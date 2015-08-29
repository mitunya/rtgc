// (C) Copyright 2015 by Wade L. Hennessey. All rights reserved.

/* Real time garbage collector running on one or more threads/cores */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <sys/time.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include "compat.h"
#include "mem-config.h"
#include "infoBits.h"
#include "mem-internals.h"
#include "vizmem.h"
#include "allocate.h"

/* Global GC variables follow. We do NOT want GC info containing
   heap pointers in the global data section, or the GC will
   mistake them for mutator pointers and save them! Hence we
   malloc some structures */

int gc_count;
double total_gc_time_in_cycle;
double max_increment_in_cycle;
double total_write_barrier_time_in_cycle;
struct timeval start_gc_cycle_time;
double last_cycle_ms;
double last_gc_ms;
double last_write_barrier_ms;

static
void remove_object_from_free_list(GPTR group, GCPTR object) {
  GCPTR prev = GET_LINK_POINTER(object->prev);
  GCPTR next = GET_LINK_POINTER(object->next);

  if (object == group->free) {
    // caller must hold green lock from group to save
    // us from repeatedly locking and unlocking for a page of objects
    group->free = next;	       // must be locked
  }
  if (object == group->black) {
    group->black = next;       // safe to not lock
  }

  if (object == group->free_last) {
    group->free_last = ((next == NULL) ? prev : next);
  }

  if (prev != NULL) {
    SET_LINK_POINTER(prev->next, next);
  }
  if (next != NULL) {
    SET_LINK_POINTER(next->prev, prev);
  }

  // must lock these
  group->green_count = group->green_count - 1;
  group->total_object_count = group->total_object_count - 1;
}

static
void convert_free_to_empty_pages(int first_page, int page_count) {
  int next_page_index = first_page;
  int end_page = first_page + page_count;
  HOLE_PTR new;

  /* Remove objects on pages from their respective free lists */
  while (next_page_index < end_page) {
    GPTR group = pages[next_page_index].group;
    int total_pages = MAX(1,group->size / BYTES_PER_PAGE);
    int object_count = (total_pages * BYTES_PER_PAGE) / group->size;
    int i;
    GCPTR next;

    next = (GCPTR) PAGE_INDEX_TO_PTR(next_page_index);
    for (i = 0; i < object_count; i++) {
      remove_object_from_free_list(group, next);
      next = (GCPTR) ((BPTR) next + group->size);
    }
    next_page_index = next_page_index + total_pages;
  }
  SXinit_empty_pages(first_page, page_count, HEAP_SEGMENT);
}

// should be no locking needed here - we're only identifying potentially
// free pages that the allocator can't use yet because we haven't converted
// them from free objects to a hole in the empty pages list.
static
void coalesce_segment_free_pages(int segment) {
  int first_page_index = -1;
  int contig_count = 0;
  int next_page_index = PTR_TO_PAGE_INDEX(segments[segment].first_segment_ptr);
  int last_page_index = PTR_TO_PAGE_INDEX(segments[segment].last_segment_ptr);
  while (next_page_index < last_page_index) {
    GPTR group = pages[next_page_index].group;
    int total_pages = (group > 0) ?
                      MAX(1, group->size / BYTES_PER_PAGE) :
                      1;
    int count_free_page = (group != EMPTY_PAGE) &&
                          (pages[next_page_index].bytes_used == 0);
    if (count_free_page) {
      if (first_page_index == -1) {
	first_page_index = next_page_index;
      }
      contig_count = contig_count + total_pages;
    }
    next_page_index = next_page_index + total_pages;
    if ((!count_free_page || next_page_index == last_page_index) &&
	(first_page_index != -1)) {
      convert_free_to_empty_pages(first_page_index, contig_count);
      first_page_index = -1;
      contig_count = 0;
    }
  }
}

static
void coalesce_all_free_pages() {
  for (int segment = 0; segment < total_segments; segment++) {
    if (segments[segment].type == HEAP_SEGMENT) {
      coalesce_segment_free_pages(segment);
    }
  }
}

/* HEY! pass in group info! */
static
int SXmake_object_gray(GCPTR current, BPTR raw) {
  GPTR group = PTR_TO_GROUP(current);
  BPTR header = (BPTR) current + sizeof(GC_HEADER);
  
  /* Only allow interior pointers to retain objects <= 1 page in size */
  if ((group->size <= INTERIOR_PTR_RETENTION_LIMIT) ||
      (((long) raw) == -1) || (raw == header)) {
    GCPTR prev = GET_LINK_POINTER(current->prev);
    GCPTR next = GET_LINK_POINTER(current->next);

    /* Remove current from WHITE space */
    if (current == group->white) {
      group->white = next;
    }
    if (prev != NULL) {
      SET_LINK_POINTER(prev->next, next);
    }
    if (next != NULL) {
      SET_LINK_POINTER(next->prev, prev);
    }

    /* Link current onto the end of the gray set. This give us a breadth
       first search when scanning the gray set (not that it matters) */
    SET_LINK_POINTER(current->prev, NULL);
    GCPTR gray = group->gray;
    if (gray == NULL) {
      SET_LINK_POINTER(current->next, group->black);
      if (group->black == NULL) {
	group->black = current;
	group->free_last = current;
      } else {
	SET_LINK_POINTER((group->black)->prev, current);
      }
    } else {
      SET_LINK_POINTER(current->next, gray);
      SET_LINK_POINTER(gray->prev, current);
    }
    SET_COLOR(current, GRAY);
    group->gray = current;
    group->white_count = group->white_count - 1;
  }
  return(group->size);
}

/* Scan memory looking for *possible* pointers */
void scan_memory_segment(BPTR low, BPTR high) {
  /* if GC_POINTER_ALIGNMENT is < 4, avoid scanning potential pointers that
     extend past the end of this object */
  high = high - sizeof(LPTR) + 1;
  for (BPTR next = low; next < high; next = next + GC_POINTER_ALIGNMENT) {
    MAYBE_PAUSE_GC;
    BPTR ptr = *((BPTR *) next);
    if (IN_PARTITION(ptr)) {
      int page_index = PTR_TO_PAGE_INDEX(ptr);
      GPTR group = pages[page_index].group;
      if (group > EXTERNAL_PAGE) {
	GCPTR gcptr = interior_to_gcptr(ptr); /* Map it ourselves here! */
	if WHITEP(gcptr) {
	    WITH_LOCK(make_object_gray_lock,
		      SXmake_object_gray(gcptr, ptr););
	}
      } else {
	if (VISUAL_MEMORY_ON && (group == EMPTY_PAGE)) {
	  SXupdate_visual_fake_ptr_page(page_index);
	}
      }
    }
  }
}

static
void scan_memory_segment_with_metadata(BPTR low, BPTR high, MetaData *md) {
  scan_memory_segment(low, high);
}

/* Snapshot-at-gc-start write barrier.
   This is really just a specialized version of scan_memory_segment. */
void * SXwrite_barrier(void *lhs_address, void *rhs) {
  if (memory_mutex == 1) {
    Debugger("HEY! write_barrier called from within GC!\n");
  }
  if (enable_write_barrier) {
    //if (ENABLE_VISUAL_MEMORY) START_CODE_TIMING;
    BPTR object = *((BPTR *) lhs_address);
    if (IN_HEAP(object)) {
      GCPTR gcptr = interior_to_gcptr(object); 
      if WHITEP(gcptr) {
	  WITH_LOCK(make_object_gray_lock,
		    SXmake_object_gray(gcptr, (BPTR) -1););
	}
    }
    //if (ENABLE_VISUAL_MEMORY) 
    //END_CODE_TIMING(total_write_barrier_time_in_cycle);
  }
  return((void *) (*(LPTR)lhs_address = (long) rhs));
}

void * SXsafe_bash(void * lhs_address, void * rhs) {
  BPTR object;
  GCPTR gcptr;

  if (CHECK_BASH) {
    object = *((BPTR *) lhs_address);
    if (IN_HEAP(object)) {
      gcptr = interior_to_gcptr(object); 
      if WHITEP(gcptr) {
	if (memory_mutex == 1) {
	  printf("HEY! write_barrier called from within GC!\n");
	  /* Call Debugger, then
	     return((void *) (*lhs_address = rhs)); */
	}
	/* Debugger */
      }
    }
  }
  /* Why is the calling the write barrier instead of simply doing the
     assignment?? */
  return(SXwrite_barrier(lhs_address, rhs));
}

void *  SXsafe_setfInit(void * lhs_address, void * rhs) {
  if (CHECK_SETFINIT) {
    BPTR object = *((BPTR *) lhs_address);
    if (object != NULL) {
      /* if ((int) object != rhs) */
      Debugger("SXsafe_setfInit problem\n");
    }
  }
  return((void *) (* (LPTR) lhs_address = (long) rhs));
}

void *ptrcpy(void *p1, void *p2, int num_bytes) {
  if (enable_write_barrier) {
    if (ENABLE_GC_TIMING) START_CODE_TIMING;
    //pause_ok_flag = 0;
    scan_memory_segment(p1, (BPTR) p1 + num_bytes);
    //pause_ok_flag = 1;
    if (ENABLE_GC_TIMING) END_CODE_TIMING(total_write_barrier_time_in_cycle);
  }
  memcpy(p1, p2, num_bytes);
  return(p1);
}

void *ptrset(void *p1, int data, int num_bytes) {
  if (enable_write_barrier) {
    //pause_ok_flag = 0;
    scan_memory_segment(p1, (BPTR) p1 + num_bytes);
    //pause_ok_flag = 1;
  }
  memset(p1, data, num_bytes);
  return(p1);
}

static
void scan_thread_registers(int thread) {
  // HEY! just scan saved regs that need it, not all 23 of them
  BPTR registers = (BPTR) threads[thread].registers;
  scan_memory_segment(registers, registers + (23 * sizeof(long)));
}

static
void scan_thread_saved_stack(int thread) {
  BPTR top = (BPTR) threads[thread].saved_stack_base;
  BPTR bottom = top + threads[thread].saved_stack_size;
  BPTR ptr_aligned_top = (BPTR) ((long) top & ~(GC_POINTER_ALIGNMENT - 1));
  scan_memory_segment(ptr_aligned_top, bottom);
}

static
void scan_thread(int thread) {
  scan_thread_registers(thread);
  scan_thread_saved_stack(thread);
}

static
void scan_threads() {
  // HEY! need to pass along number of threads scanned
  for (int next_thread = 1; next_thread < total_threads; next_thread++) {
    scan_thread(next_thread);
  }
}
  
static
void scan_global_roots() {
  for (int i = 0; i < total_global_roots; i++) {
    BPTR ptr =  *((BPTR *) *(global_roots + i));
    if (IN_PARTITION(ptr)) {
      int page_index = PTR_TO_PAGE_INDEX(ptr);
      GPTR group = pages[page_index].group;
      if (group > EXTERNAL_PAGE) {
	GCPTR gcptr = interior_to_gcptr(ptr); /* Map it ourselves here! */
	if WHITEP(gcptr) {
	    WITH_LOCK(make_object_gray_lock,
		      SXmake_object_gray(gcptr, ptr););
	  }
      } else {
	if (VISUAL_MEMORY_ON && (group == EMPTY_PAGE)) {
	  SXupdate_visual_fake_ptr_page(page_index);
	}
      }
    }
  }
}

static
void scan_static_space() {
  //  BPTR next, low, end;
  //  GCPTR gcptr;
  // int size;

  BPTR next = first_static_ptr;
  BPTR end = last_static_ptr;
  while (next < end) {
    int size = *((int *) next);
    BPTR low = next + sizeof(GCPTR);
    size = size >> LINK_INFO_BITS;
    next = low + size;
    GCPTR gcptr = (GCPTR) (low - sizeof(GC_HEADER));
    scan_object(gcptr, size + sizeof(GC_HEADER));
    /* Delete ME! HEY! Convert to common scanner with scan_object 
       if (GET_STORAGE_CLASS(gcptr) != SC_NOPOINTERS) {
       scan_memory_segment(low,next);
       } */
  }
}

static
void scan_root_set() {
  last_gc_state = "Scan Threads";
  UPDATE_VISUAL_STATE();
  scan_threads();
  last_gc_state = "Scan Globals";
  UPDATE_VISUAL_STATE();
  scan_global_roots();
  last_gc_state = "Scan Statics";
  UPDATE_VISUAL_STATE();
  scan_static_space();
}

void scan_object(GCPTR ptr, int total_size) {
  BPTR bptr, low, high;

  bptr = (BPTR) ptr;
  low = bptr + sizeof(GC_HEADER);
  high = bptr + total_size;
  switch (GET_STORAGE_CLASS(ptr)) {
  case SC_NOPOINTERS: break;
  case SC_POINTERS:
    scan_memory_segment(low, high);
    break;
  case SC_METADATA:
    scan_memory_segment_with_metadata(low, high, 0);
    break;
  case SC_INSTANCE:
    /* instance_metadata((SXobject) low); */
    scan_memory_segment_with_metadata(low,high,0);
    break;
  default: Debugger(0);
  }
}

static
void scan_object_with_group(GCPTR ptr, GPTR group) {
  scan_object(ptr, group->size);
  SET_COLOR(ptr,marked_color);
  group->black = ptr;
  group->black_count = group->black_count + 1;
}

/* HEY! Fix this up now that it's not continuation based... */
static
void scan_gray_set() {
  int i, scan_count, rescan_all_groups;

  last_gc_state = "Scan Gray Set";
  UPDATE_VISUAL_STATE();
  i = MIN_GROUP_INDEX;
  scan_count = 0;
  do {
    while (i <= MAX_GROUP_INDEX) {
      GPTR group = &groups[i];
      GCPTR current = group->black;
      /* current could be gray, black, or green */
      if ((current != NULL ) && (!(GRAYP(current)))) {
	current = GET_LINK_POINTER(current->prev);
      }
      while (current != NULL) {
	MAYBE_PAUSE_GC;
	scan_object_with_group(current,group);
	scan_count = scan_count + 1;
	current = GET_LINK_POINTER(current->prev);
      }
      i = i + 1;
    }
    if (scan_count > 0) {
      rescan_all_groups = 1;
      i = MIN_GROUP_INDEX;
      scan_count = 0;

    } else {
      rescan_all_groups = 0;
    }
  } while (rescan_all_groups == 1);
  MAYBE_PAUSE_GC;
}

static
void flip() {
  MAYBE_PAUSE_GC;
  // Originally, at this point all mutator threads are stopped, and none of
  // them is in the middle of an SXallocate. We got this for free by being
  // single threaded and implicity locking by yielding only when we chose to.
  //
  // Now, we have to acquire all group allocation locks to be sure no mutator
  // is allocating. Then we have to interrupt all mutators to stop them.
  // Then we can proceed to flip and then resume when the flip is done.
  last_gc_state = "Flip";
  for (int i = MIN_GROUP_INDEX; i <= MAX_GROUP_INDEX; i++) {
    GPTR group = &groups[i];
    // No allocation allowed during a flip
    pthread_mutex_lock(&(group->free_lock));
		       
    group->gray = NULL;
    GCPTR free = group->free;
    if (free != NULL) {
      GCPTR prev = GET_LINK_POINTER(free->prev);
      if (prev != NULL) {
	SET_LINK_POINTER(prev->next,NULL); /* end black set */
      }
      SET_LINK_POINTER(free->prev,NULL);
    } else {
      GCPTR free_last = group->free_last;
      if (free_last != NULL) {
	SET_LINK_POINTER(free_last->next,NULL); /* end black set */
      }
      group->free_last = NULL;
    }
    
    GCPTR black = group->black;
    if (black == NULL) {
      // HEY! Why can't black be null?
      //printf("YOW! black is NULL\n");
    } else {
      // HEY! this was unconditional before - why?
      group->white = (GREENP(black) ? NULL : black);
    }

    // Why do we make black point at free? Why not always make black
    // null here?
    group->black = group->free;
    group->white_count = group->black_count;
    group->black_count = 0;
  }

  // we can safely do this without an explicit lock because we're holding
  // all the green_locks right now, and the write barrier is off.
  assert(0 == enable_write_barrier);
  SWAP(marked_color,unmarked_color);
  stop_all_mutators_and_save_state();

  for (int i = MIN_GROUP_INDEX; i <= MAX_GROUP_INDEX; i++) {
    GPTR group = &groups[i];
    pthread_mutex_unlock(&(group->free_lock));
  }

}

/* We need to change garbage color to green now so conservative
   scanning in the next gc cyclse doesn't start making free objects 
   that look white turn gray! */
static
GCPTR recycle_group_garbage(GPTR group) {
  int count = 0;
  GCPTR last = NULL;
  GCPTR next = group->white;

  while (next != NULL) {
    int page_index = PTR_TO_PAGE_INDEX(next);
    PPTR page = &pages[page_index];
    int old_bytes_used = page->bytes_used;
    page->bytes_used = page->bytes_used - group->size;
    if (VISUAL_MEMORY_ON) {
      SXmaybe_update_visual_page(page_index,old_bytes_used,page->bytes_used);
    }
    /* Finalize code was here. Maybe add new finalize code some day */

    SET_COLOR(next,GREEN);
    if (DETECT_INVALID_REFS) {
      memset((BPTR) next + 8, INVALID_ADDRESS, group->size);
    }
    last = next;
    next = GET_LINK_POINTER(next->next);
    count = count + 1;
    MAYBE_PAUSE_GC;
  }

  /* HEY! could unlink free obj on pages whoe count is 0. Then hook remaining
     frag free onto free list and coalesce 0 pages */
  if (count != group->white_count) {
    verify_all_groups();
    Debugger(0);
  }
  // Append garbage to free list. Not great for a VM system, but it's easier
  if (last != NULL) {
    SET_LINK_POINTER(last->next, NULL);

    // HEY! need free_lock for this
    if (group->free == NULL) {
      group->free = group->white;
    }
    // HEY! need lock for this unless free_lock covers black too
    if (group->black == NULL) {
      group->black = group->white;
    }
    
    if (group->free_last != NULL) {
      SET_LINK_POINTER((group->free_last)->next, group->white);
    }
    SET_LINK_POINTER((group->white)->prev, group->free_last);
    group->free_last = last;
    group->green_count = group->green_count + count;
  }
  group->white = NULL;
  group->white_count = 0;
  return(last);
}

static 
void recycle_all_garbage() {
  last_gc_state = "Recycle Garbage";
  UPDATE_VISUAL_STATE();
  for (int i = MIN_GROUP_INDEX; i <= MAX_GROUP_INDEX; i++) {
    recycle_group_garbage(&groups[i]);
  }
  coalesce_all_free_pages();
}

static 
void reset_gc_cycle_stats() {
  total_allocation_this_cycle = 0;
  total_gc_time_in_cycle = 0.0;
  total_write_barrier_time_in_cycle = 0.0;
  max_increment_in_cycle = 0.0;
  if (ENABLE_GC_TIMING) {
    gettimeofday(&start_gc_cycle_time, 0);
  }
}

static 
void summarize_gc_cycle_stats() {
  double total_cycle_time;
  
  if (ENABLE_GC_TIMING) {
    ELAPSED_MILLISECONDS(start_gc_cycle_time, total_cycle_time);
    last_cycle_ms = total_cycle_time;
    last_gc_ms = total_gc_time_in_cycle;
    last_write_barrier_ms = total_write_barrier_time_in_cycle;
  }
  if (VISUAL_MEMORY_ON) SXdraw_visual_gc_stats();
}

static
void full_gc() {
  reset_gc_cycle_stats();
  flip();
  scan_root_set();
  scan_gray_set();
  
  enable_write_barrier = 0;
  recycle_all_garbage(0);
  // move this into 
  // enable_write_barrier = 1;

  gc_count = gc_count + 1;
  summarize_gc_cycle_stats();
  last_gc_state = "Cycle Complete";
  UPDATE_VISUAL_STATE();
}

void rtgc_loop() {
  while (1) {
    while (0 == run_gc);
    printf("gc start...");
    fflush(stdout);
    full_gc();
    printf("gc end - gc_count %d\n", gc_count);
    fflush(stdout);
    run_gc = 0;
  }
}

int rtgc_count(void) {
  return(gc_count);
}

void init_realtime_gc() {
  // the gc_flip signal handler uses this to find the thread_index of 
  // the mutator thread it is running on
  if (0 != pthread_key_create(&thread_index_key, NULL)) {
    printf("thread_index_key create failed!\n");
  }

  total_global_roots = 0;
  gc_count = 0;
  visual_memory_on = 0;
  last_gc_state = "<initial state>";
  pthread_mutex_init(&total_threads_lock, NULL);
  pthread_mutex_init(&empty_pages_lock, NULL);
  pthread_mutex_init(&make_object_gray_lock, NULL);
  sem_init(&gc_semaphore, 0, 0);
  init_signals_for_rtgc();
  counter_init(&stacks_copied_counter);
}
