// Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "kqueue.h"
#include "selector.h"
#include "list.h"
#include "array.h"

#include <sys/event.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>

enum {
	DEBUG = 0,
	DEBUG_IO_READ = 0,
	DEBUG_IO_WRITE = 0,
	DEBUG_IO_WAIT = 0
};

static VALUE IO_Event_Selector_KQueue = Qnil;

enum {KQUEUE_MAX_EVENTS = 64};

// This represents an actual fiber waiting for a specific event.
struct IO_Event_Selector_KQueue_Waiting {
	struct IO_Event_List list;
	
	// The events the fiber is waiting for.
	enum IO_Event events;
	
	// The fiber value itself.
	VALUE fiber;
};

struct IO_Event_Selector_KQueue {
	struct IO_Event_Selector backend;
	int descriptor;
	int blocked;
	
	struct IO_Event_Array descriptors;
};

void IO_Event_Selector_KQueue_Type_mark(void *_selector)
{
	struct IO_Event_Selector_KQueue *selector = _selector;
	IO_Event_Selector_mark(&selector->backend);
}

static
void close_internal(struct IO_Event_Selector_KQueue *selector) {
	if (selector->descriptor >= 0) {
		close(selector->descriptor);
		selector->descriptor = -1;
	}
}

void IO_Event_Selector_KQueue_Type_free(void *_selector)
{
	struct IO_Event_Selector_KQueue *selector = _selector;
	
	close_internal(selector);
	
	IO_Event_Array_free(&selector->descriptors);
	
	free(selector);
}

size_t IO_Event_Selector_KQueue_Type_size(const void *selector)
{
	return sizeof(struct IO_Event_Selector_KQueue);
}

static const rb_data_type_t IO_Event_Selector_KQueue_Type = {
	.wrap_struct_name = "IO_Event::Backend::KQueue",
	.function = {
		.dmark = IO_Event_Selector_KQueue_Type_mark,
		.dfree = IO_Event_Selector_KQueue_Type_free,
		.dsize = IO_Event_Selector_KQueue_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

// This represents zero or more fibers waiting for a specific descriptor.
struct IO_Event_Selector_KQueue_Descriptor {
	struct IO_Event_List list;
	
	// The events that are currently ready:
	enum IO_Event ready;
};

inline static
struct IO_Event_Selector_KQueue_Descriptor * IO_Event_Selector_KQueue_Descriptor_lookup(struct IO_Event_Selector_KQueue *selector, int descriptor)
{
	struct IO_Event_Selector_KQueue_Descriptor *kqueue_descriptor = IO_Event_Array_lookup(&selector->descriptors, descriptor);
	
	if (!kqueue_descriptor) {
		rb_sys_fail("IO_Event_Selector_KQueue_Descriptor_lookup:IO_Event_Array_lookup");
	}
	
	return kqueue_descriptor;
}

void IO_Event_Selector_KQueue_Descriptor_initialize(void *element)
{
	struct IO_Event_Selector_KQueue_Descriptor *kqueue_descriptor = element;
	IO_Event_List_initialize(&kqueue_descriptor->list);
	kqueue_descriptor->ready = 0;
}

void IO_Event_Selector_KQueue_Descriptor_free(void *element)
{
	struct IO_Event_Selector_KQueue_Descriptor *kqueue_descriptor = element;
	
	IO_Event_List_free(&kqueue_descriptor->list);
}

VALUE IO_Event_Selector_KQueue_allocate(VALUE self) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	IO_Event_Selector_initialize(&selector->backend, Qnil);
	selector->descriptor = -1;
	selector->blocked = 0;
	
	selector->descriptors.element_initialize = IO_Event_Selector_KQueue_Descriptor_initialize;
	selector->descriptors.element_free = IO_Event_Selector_KQueue_Descriptor_free;
	IO_Event_Array_allocate(&selector->descriptors, 1024, sizeof(struct IO_Event_Selector_KQueue_Descriptor));
	
	return instance;
}

VALUE IO_Event_Selector_KQueue_initialize(VALUE self, VALUE loop) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	IO_Event_Selector_initialize(&selector->backend, loop);
	int result = kqueue();
	
	if (result == -1) {
		rb_sys_fail("IO_Event_Selector_KQueue_initialize:kqueue");
	} else {
		// Make sure the descriptor is closed on exec.
		ioctl(result, FIOCLEX);
		
		selector->descriptor = result;
		
		rb_update_max_fd(selector->descriptor);
	}
	
	return self;
}

VALUE IO_Event_Selector_KQueue_loop(VALUE self) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	return selector->backend.loop;
}

VALUE IO_Event_Selector_KQueue_close(VALUE self) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	close_internal(selector);
	
	return Qnil;
}

VALUE IO_Event_Selector_KQueue_transfer(VALUE self)
{
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	return IO_Event_Selector_fiber_transfer(selector->backend.loop, 0, NULL);
}

VALUE IO_Event_Selector_KQueue_resume(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	return IO_Event_Selector_resume(&selector->backend, argc, argv);
}

VALUE IO_Event_Selector_KQueue_yield(VALUE self)
{
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	return IO_Event_Selector_yield(&selector->backend);
}

VALUE IO_Event_Selector_KQueue_push(VALUE self, VALUE fiber)
{
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	IO_Event_Selector_queue_push(&selector->backend, fiber);
	
	return Qnil;
}

VALUE IO_Event_Selector_KQueue_raise(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	return IO_Event_Selector_raise(&selector->backend, argc, argv);
}

VALUE IO_Event_Selector_KQueue_ready_p(VALUE self) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	return selector->backend.ready ? Qtrue : Qfalse;
}

inline static
int IO_Event_Selector_KQueue_arm(struct IO_Event_Selector_KQueue *selector, uintptr_t ident, struct IO_Event_Selector_KQueue_Descriptor *kqueue_descriptor, struct IO_Event_Selector_KQueue_Waiting *waiting)
{
	int count = 0;
	struct kevent kevents[3] = {0};
	
	enum IO_Event events = waiting->events;
	
	if (events & IO_EVENT_READABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_READ;
		kevents[count].flags = EV_ADD | EV_ENABLE | EV_ONESHOT;
		kevents[count].udata = (void *)kqueue_descriptor;
		
// #ifdef EV_OOBAND
// 		if (events & IO_EVENT_PRIORITY) {
// 			kevents[count].flags |= EV_OOBAND;
// 		}
// #endif
		
		count++;
	}
	
	if (events & IO_EVENT_WRITABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_WRITE;
		kevents[count].flags = EV_ADD | EV_ENABLE | EV_ONESHOT;
		kevents[count].udata = (void *)kqueue_descriptor;
		count++;
	}
	
	if (events & IO_EVENT_EXIT) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_PROC;
		kevents[count].flags = EV_ADD | EV_ENABLE | EV_ONESHOT;
		kevents[count].fflags = NOTE_EXIT;
		kevents[count].udata = (void *)kqueue_descriptor;
		count++;
	}
	
	int result = kevent(selector->descriptor, kevents, count, NULL, 0, NULL);
	
	if (result == -1) {
		// No such process - the process has probably already terminated:
		// if (errno == ESRCH) {
		// 	return 0;
		// }
		
		rb_sys_fail("IO_Event_Selector_KQueue_arm:kevent");
	}
	
	IO_Event_List_prepend(&kqueue_descriptor->list, &waiting->list);
	
	return count;
}

inline static
enum IO_Event events_from_kevent_filter(int filter) {
	switch (filter) {
		case EVFILT_READ:
			return IO_EVENT_READABLE;
		case EVFILT_WRITE:
			return IO_EVENT_WRITABLE;
		case EVFILT_PROC:
			return IO_EVENT_EXIT;
		default:
			return 0;
	}
}

struct process_wait_arguments {
	struct IO_Event_Selector_KQueue *selector;
	struct IO_Event_Selector_KQueue_Waiting *waiting;
	pid_t pid;
};

static
VALUE process_wait_transfer(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	IO_Event_Selector_fiber_transfer(arguments->selector->backend.loop, 0, NULL);
	
	return IO_Event_Selector_process_status_wait(arguments->pid);
}

static
VALUE process_wait_ensure(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	IO_Event_List_pop(&arguments->waiting->list);
	
	return Qnil;
}

VALUE IO_Event_Selector_KQueue_process_wait(VALUE self, VALUE fiber, VALUE _pid, VALUE _flags) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	pid_t pid = NUM2PIDT(_pid);
	
	struct IO_Event_Selector_KQueue_Descriptor *kqueue_descriptor = IO_Event_Selector_KQueue_Descriptor_lookup(selector, pid);
	
	struct IO_Event_Selector_KQueue_Waiting waiting = {
		.fiber = fiber,
		.events = IO_EVENT_EXIT,
	};
	
	struct process_wait_arguments process_wait_arguments = {
		.selector = selector,
		.waiting = &waiting,
		.pid = pid,
	};
	
	IO_Event_Selector_KQueue_arm(selector, pid, kqueue_descriptor, &waiting);
	
	return rb_ensure(process_wait_transfer, (VALUE)&process_wait_arguments, process_wait_ensure, (VALUE)&process_wait_arguments);
}

struct io_wait_arguments {
	struct IO_Event_Selector_KQueue *selector;
	struct IO_Event_Selector_KQueue_Waiting *waiting;
};

static
VALUE io_wait_ensure(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	IO_Event_List_pop(&arguments->waiting->list);
	
	return Qnil;
}

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	VALUE result = IO_Event_Selector_fiber_transfer(arguments->selector->backend.loop, 0, NULL);
	
	// If the fiber is being cancelled, it might be resumed with nil:
	if (!RTEST(result)) {
		return Qfalse;
	}
	
	return result;
}

VALUE IO_Event_Selector_KQueue_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	int descriptor = IO_Event_Selector_io_descriptor(io);
	struct IO_Event_Selector_KQueue_Descriptor *kqueue_descriptor = IO_Event_Array_lookup(&selector->descriptors, descriptor);
	
	struct IO_Event_Selector_KQueue_Waiting waiting = {
		.fiber = fiber,
		.events = RB_NUM2INT(events),
	};
	
	IO_Event_Selector_KQueue_arm(selector, descriptor, kqueue_descriptor, &waiting);
	
	struct io_wait_arguments io_wait_arguments = {
		.selector = selector,
		.waiting = &waiting,
	};
	
	if (DEBUG_IO_WAIT) fprintf(stderr, "IO_Event_Selector_KQueue_io_wait descriptor=%d\n", descriptor);
	
	return rb_ensure(io_wait_transfer, (VALUE)&io_wait_arguments, io_wait_ensure, (VALUE)&io_wait_arguments);
}

#ifdef HAVE_RUBY_IO_BUFFER_H

struct io_read_arguments {
	VALUE self;
	VALUE fiber;
	VALUE io;
	
	int flags;
	
	int descriptor;
	
	VALUE buffer;
	size_t length;
	size_t offset;
};

static
VALUE io_read_loop(VALUE _arguments) {
	struct io_read_arguments *arguments = (struct io_read_arguments *)_arguments;
	
	void *base;
	size_t size;
	rb_io_buffer_get_bytes_for_writing(arguments->buffer, &base, &size);
	
	size_t length = arguments->length;
	size_t offset = arguments->offset;
	size_t total = 0;
	
	if (DEBUG_IO_READ) fprintf(stderr, "io_read_loop(fd=%d, length=%zu)\n", arguments->descriptor, length);
	
	size_t maximum_size = size - offset;
	while (maximum_size) {
		if (DEBUG_IO_READ) fprintf(stderr, "read(%d, +%ld, %ld)\n", arguments->descriptor, offset, maximum_size);
		ssize_t result = read(arguments->descriptor, (char*)base+offset, maximum_size);
		if (DEBUG_IO_READ) fprintf(stderr, "read(%d, +%ld, %ld) -> %zd\n", arguments->descriptor, offset, maximum_size, result);
		
		if (result > 0) {
			total += result;
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(errno)) {
			if (DEBUG_IO_READ) fprintf(stderr, "IO_Event_Selector_KQueue_io_wait(fd=%d, length=%zu)\n", arguments->descriptor, length);
			IO_Event_Selector_KQueue_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(IO_EVENT_READABLE));
		} else {
			if (DEBUG_IO_READ) fprintf(stderr, "io_read_loop(fd=%d, length=%zu) -> errno=%d\n", arguments->descriptor, length, errno);
			return rb_fiber_scheduler_io_result(-1, errno);
		}
		
		maximum_size = size - offset;
	}
	
	if (DEBUG_IO_READ) fprintf(stderr, "io_read_loop(fd=%d, length=%zu) -> %zu\n", arguments->descriptor, length, offset);
	return rb_fiber_scheduler_io_result(total, 0);
}

static
VALUE io_read_ensure(VALUE _arguments) {
	struct io_read_arguments *arguments = (struct io_read_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
}

VALUE IO_Event_Selector_KQueue_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length, VALUE _offset) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	int descriptor = IO_Event_Selector_io_descriptor(io);
	
	size_t length = NUM2SIZET(_length);
	size_t offset = NUM2SIZET(_offset);
	
	struct io_read_arguments io_read_arguments = {
		.self = self,
		.fiber = fiber,
		.io = io,
		
		.flags = IO_Event_Selector_nonblock_set(descriptor),
		.descriptor = descriptor,
		.buffer = buffer,
		.length = length,
		.offset = offset,
	};
	
	return rb_ensure(io_read_loop, (VALUE)&io_read_arguments, io_read_ensure, (VALUE)&io_read_arguments);
}

static VALUE IO_Event_Selector_KQueue_io_read_compatible(int argc, VALUE *argv, VALUE self)
{
	rb_check_arity(argc, 4, 5);
	
	VALUE _offset = SIZET2NUM(0);
	
	if (argc == 5) {
		_offset = argv[4];
	}
	
	return IO_Event_Selector_KQueue_io_read(self, argv[0], argv[1], argv[2], argv[3], _offset);
}

struct io_write_arguments {
	VALUE self;
	VALUE fiber;
	VALUE io;
	
	int flags;
	
	int descriptor;
	
	VALUE buffer;
	size_t length;
	size_t offset;
};

static
VALUE io_write_loop(VALUE _arguments) {
	struct io_write_arguments *arguments = (struct io_write_arguments *)_arguments;
	
	const void *base;
	size_t size;
	rb_io_buffer_get_bytes_for_reading(arguments->buffer, &base, &size);
	
	size_t length = arguments->length;
	size_t offset = arguments->offset;
	size_t total = 0;
	
	if (length > size) {
		rb_raise(rb_eRuntimeError, "Length exceeds size of buffer!");
	}
	
	if (DEBUG_IO_WRITE) fprintf(stderr, "io_write_loop(fd=%d, length=%zu)\n", arguments->descriptor, length);
	
	size_t maximum_size = size - offset;
	while (maximum_size) {
		if (DEBUG_IO_WRITE) fprintf(stderr, "write(%d, +%ld, %ld, length=%zu)\n", arguments->descriptor, offset, maximum_size, length);
		ssize_t result = write(arguments->descriptor, (char*)base+offset, maximum_size);
		if (DEBUG_IO_WRITE) fprintf(stderr, "write(%d, +%ld, %ld) -> %zd\n", arguments->descriptor, offset, maximum_size, result);
		
		if (result > 0) {
			total += result;
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(errno)) {
			if (DEBUG_IO_WRITE) fprintf(stderr, "IO_Event_Selector_KQueue_io_wait(fd=%d, length=%zu)\n", arguments->descriptor, length);
			IO_Event_Selector_KQueue_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(IO_EVENT_READABLE));
		} else {
			if (DEBUG_IO_WRITE) fprintf(stderr, "io_write_loop(fd=%d, length=%zu) -> errno=%d\n", arguments->descriptor, length, errno);
			return rb_fiber_scheduler_io_result(-1, errno);
		}
		
		maximum_size = size - offset;
	}
	
	if (DEBUG_IO_READ) fprintf(stderr, "io_write_loop(fd=%d, length=%zu) -> %zu\n", arguments->descriptor, length, offset);
	return rb_fiber_scheduler_io_result(total, 0);
};

static
VALUE io_write_ensure(VALUE _arguments) {
	struct io_write_arguments *arguments = (struct io_write_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
};

VALUE IO_Event_Selector_KQueue_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length, VALUE _offset) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	int descriptor = IO_Event_Selector_io_descriptor(io);
	
	size_t length = NUM2SIZET(_length);
	size_t offset = NUM2SIZET(_offset);
	
	struct io_write_arguments io_write_arguments = {
		.self = self,
		.fiber = fiber,
		.io = io,
		
		.flags = IO_Event_Selector_nonblock_set(descriptor),
		.descriptor = descriptor,
		.buffer = buffer,
		.length = length,
		.offset = offset,
	};
	
	return rb_ensure(io_write_loop, (VALUE)&io_write_arguments, io_write_ensure, (VALUE)&io_write_arguments);
}

static VALUE IO_Event_Selector_KQueue_io_write_compatible(int argc, VALUE *argv, VALUE self)
{
	rb_check_arity(argc, 4, 5);
	
	VALUE _offset = SIZET2NUM(0);
	
	if (argc == 5) {
		_offset = argv[4];
	}
	
	return IO_Event_Selector_KQueue_io_write(self, argv[0], argv[1], argv[2], argv[3], _offset);
}

#endif

static
struct timespec * make_timeout(VALUE duration, struct timespec * storage) {
	if (duration == Qnil) {
		return NULL;
	}
	
	if (FIXNUM_P(duration)) {
		storage->tv_sec = NUM2TIMET(duration);
		storage->tv_nsec = 0;
		
		return storage;
	}
	
	else if (RB_FLOAT_TYPE_P(duration)) {
		double value = RFLOAT_VALUE(duration);
		time_t seconds = value;
		
		storage->tv_sec = seconds;
		storage->tv_nsec = (value - seconds) * 1000000000L;
		
		return storage;
	}
	
	rb_raise(rb_eRuntimeError, "unable to convert timeout");
}

static
int timeout_nonblocking(struct timespec * timespec) {
	return timespec && timespec->tv_sec == 0 && timespec->tv_nsec == 0;
}

struct select_arguments {
	struct IO_Event_Selector_KQueue *selector;
	
	int count;
	struct kevent events[KQUEUE_MAX_EVENTS];
	
	struct timespec storage;
	struct timespec *timeout;
};

static
void * select_internal(void *_arguments) {
	struct select_arguments * arguments = (struct select_arguments *)_arguments;
	
	arguments->count = kevent(arguments->selector->descriptor, NULL, 0, arguments->events, arguments->count, arguments->timeout);
	
	return NULL;
}

static
void select_internal_without_gvl(struct select_arguments *arguments) {
	arguments->selector->blocked = 1;
	
	rb_thread_call_without_gvl(select_internal, (void *)arguments, RUBY_UBF_IO, 0);
	arguments->selector->blocked = 0;
	
	if (arguments->count == -1) {
		if (errno != EINTR) {
			rb_sys_fail("select_internal_without_gvl:kevent");
		} else {
			arguments->count = 0;
		}
	}
}

static
void select_internal_with_gvl(struct select_arguments *arguments) {
	select_internal((void *)arguments);
	
	if (arguments->count == -1) {
		if (errno != EINTR) {
			rb_sys_fail("select_internal_with_gvl:kevent");
		} else {
			arguments->count = 0;
		}
	}
}

static
void IO_Event_Selector_KQueue_handle(struct IO_Event_Selector_KQueue *selector, uintptr_t ident, struct IO_Event_Selector_KQueue_Descriptor *kqueue_descriptor)
{
	// This is the mask of all events that occured for the given descriptor:
	enum IO_Event io_event = kqueue_descriptor->ready;
	
	if (io_event) {
		kqueue_descriptor->ready = 0;
	} else {
		return;
	}
	
	struct IO_Event_List *list = &kqueue_descriptor->list;
	struct IO_Event_List *node = list->tail;
	struct IO_Event_List saved = {NULL, NULL};
	
	// It's possible (but unlikely) that the address of list will changing during iteration.
	while (node != list) {
		struct IO_Event_Selector_KQueue_Waiting *waiting = (struct IO_Event_Selector_KQueue_Waiting *)node;
		
		enum IO_Event matching_events = waiting->events & io_event;
		
		if (DEBUG) fprintf(stderr, "IO_Event_Selector_KQueue_handle: ident=%lu, events=%d, matching_events=%d\n", ident, io_event, matching_events);
		
		if (matching_events) {
			IO_Event_List_append(node, &saved);
			
			VALUE argument = RB_INT2NUM(matching_events);
			IO_Event_Selector_fiber_transfer(waiting->fiber, 1, &argument);
			
			node = saved.tail;
			IO_Event_List_pop(&saved);
		} else {
			node = node->tail;
		}
	}
}

VALUE IO_Event_Selector_KQueue_select(VALUE self, VALUE duration) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	int ready = IO_Event_Selector_queue_flush(&selector->backend);
	
	struct select_arguments arguments = {
		.selector = selector,
		.count = KQUEUE_MAX_EVENTS,
		.storage = {
			.tv_sec = 0,
			.tv_nsec = 0
		}
	};
	
	arguments.timeout = &arguments.storage;
	
	// We break this implementation into two parts.
	// (1) count = kevent(..., timeout = 0)
	// (2) without gvl: kevent(..., timeout = 0) if count == 0 and timeout != 0
	// This allows us to avoid releasing and reacquiring the GVL.
	// Non-comprehensive testing shows this gives a 1.5x speedup.
	
	// First do the syscall with no timeout to get any immediately available events:
	if (DEBUG) fprintf(stderr, "\r\nselect_internal_with_gvl timeout=" PRINTF_TIMESPEC "\r\n", PRINTF_TIMESPEC_ARGS(arguments.storage));
	select_internal_with_gvl(&arguments);
	if (DEBUG) fprintf(stderr, "\r\nselect_internal_with_gvl done\r\n");
	
	// If we:
	// 1. Didn't process any ready fibers, and
	// 2. Didn't process any events from non-blocking select (above), and
	// 3. There are no items in the ready list,
	// then we can perform a blocking select.
	if (!ready && !arguments.count && !selector->backend.ready) {
		arguments.timeout = make_timeout(duration, &arguments.storage);
		
		if (!timeout_nonblocking(arguments.timeout)) {
			arguments.count = KQUEUE_MAX_EVENTS;
			
			if (DEBUG) fprintf(stderr, "IO_Event_Selector_KQueue_select timeout=" PRINTF_TIMESPEC "\n", PRINTF_TIMESPEC_ARGS(arguments.storage));
			select_internal_without_gvl(&arguments);
		}
	}
	
	for (int i = 0; i < arguments.count; i += 1) {
		if (arguments.events[i].udata) {
			struct IO_Event_Selector_KQueue_Descriptor *kqueue_descriptor = arguments.events[i].udata;
			kqueue_descriptor->ready |= events_from_kevent_filter(arguments.events[i].filter);
		}
	}
	
	for (int i = 0; i < arguments.count; i += 1) {
		if (arguments.events[i].udata) {
			struct IO_Event_Selector_KQueue_Descriptor *kqueue_descriptor = arguments.events[i].udata;
			IO_Event_Selector_KQueue_handle(selector, arguments.events[i].ident, kqueue_descriptor);
		}
	}
	
	return RB_INT2NUM(arguments.count);
}

VALUE IO_Event_Selector_KQueue_wakeup(VALUE self) {
	struct IO_Event_Selector_KQueue *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, selector);
	
	if (selector->blocked) {
		struct kevent trigger = {0};
		
		trigger.filter = EVFILT_USER;
		trigger.flags = EV_ADD | EV_CLEAR;
		
		int result = kevent(selector->descriptor, &trigger, 1, NULL, 0, NULL);
		
		if (result == -1) {
			rb_sys_fail("IO_Event_Selector_KQueue_wakeup:kevent");
		}
		
		// FreeBSD apparently only works if the NOTE_TRIGGER is done as a separate call.
		trigger.flags = 0;
		trigger.fflags = NOTE_TRIGGER;
		
		result = kevent(selector->descriptor, &trigger, 1, NULL, 0, NULL);
		
		if (result == -1) {
			rb_sys_fail("IO_Event_Selector_KQueue_wakeup:kevent");
		}
		
		return Qtrue;
	}
	
	return Qfalse;
}

void Init_IO_Event_Selector_KQueue(VALUE IO_Event_Selector) {
	IO_Event_Selector_KQueue = rb_define_class_under(IO_Event_Selector, "KQueue", rb_cObject);
	rb_gc_register_mark_object(IO_Event_Selector_KQueue);
	
	rb_define_alloc_func(IO_Event_Selector_KQueue, IO_Event_Selector_KQueue_allocate);
	rb_define_method(IO_Event_Selector_KQueue, "initialize", IO_Event_Selector_KQueue_initialize, 1);
	
	rb_define_method(IO_Event_Selector_KQueue, "loop", IO_Event_Selector_KQueue_loop, 0);
	
	rb_define_method(IO_Event_Selector_KQueue, "transfer", IO_Event_Selector_KQueue_transfer, 0);
	rb_define_method(IO_Event_Selector_KQueue, "resume", IO_Event_Selector_KQueue_resume, -1);
	rb_define_method(IO_Event_Selector_KQueue, "yield", IO_Event_Selector_KQueue_yield, 0);
	rb_define_method(IO_Event_Selector_KQueue, "push", IO_Event_Selector_KQueue_push, 1);
	rb_define_method(IO_Event_Selector_KQueue, "raise", IO_Event_Selector_KQueue_raise, -1);
	
	rb_define_method(IO_Event_Selector_KQueue, "ready?", IO_Event_Selector_KQueue_ready_p, 0);
	
	rb_define_method(IO_Event_Selector_KQueue, "select", IO_Event_Selector_KQueue_select, 1);
	rb_define_method(IO_Event_Selector_KQueue, "wakeup", IO_Event_Selector_KQueue_wakeup, 0);
	rb_define_method(IO_Event_Selector_KQueue, "close", IO_Event_Selector_KQueue_close, 0);
	
	rb_define_method(IO_Event_Selector_KQueue, "io_wait", IO_Event_Selector_KQueue_io_wait, 3);
	
#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(IO_Event_Selector_KQueue, "io_read", IO_Event_Selector_KQueue_io_read_compatible, -1);
	rb_define_method(IO_Event_Selector_KQueue, "io_write", IO_Event_Selector_KQueue_io_write_compatible, -1);
#endif
	
	rb_define_method(IO_Event_Selector_KQueue, "process_wait", IO_Event_Selector_KQueue_process_wait, 3);
}
