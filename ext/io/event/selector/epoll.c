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

#include <sys/epoll.h>
#include <time.h>
#include <errno.h>

#include "pidfd.c"
#include "../interrupt.h"

enum {
	DEBUG = 0,
};

static VALUE IO_Event_Selector_EPoll = Qnil;

enum {EPOLL_MAX_EVENTS = 64};

// This represents an actual fiber waiting for a specific event.
struct IO_Event_Selector_EPoll_Waiting {
	struct IO_Event_List list;
	
	// The events the fiber is waiting for.
	enum IO_Event events;
	
	// The fiber value itself.
	VALUE fiber;
};

struct IO_Event_Selector_EPoll {
	struct IO_Event_Selector backend;
	int descriptor;
	int blocked;
	
	struct IO_Event_Interrupt interrupt;
	struct IO_Event_Array descriptors;
};

void IO_Event_Selector_EPoll_Type_mark(void *_selector)
{
	struct IO_Event_Selector_EPoll *selector = _selector;
	IO_Event_Selector_mark(&selector->backend);
}

static
void close_internal(struct IO_Event_Selector_EPoll *selector) {
	if (selector->descriptor >= 0) {
		close(selector->descriptor);
		selector->descriptor = -1;
		
		IO_Event_Interrupt_close(&selector->interrupt);
	}
}

void IO_Event_Selector_EPoll_Type_free(void *_selector)
{
	struct IO_Event_Selector_EPoll *selector = _selector;
	
	close_internal(selector);
	
	IO_Event_Array_free(&selector->descriptors);
	
	free(selector);
}

size_t IO_Event_Selector_EPoll_Type_size(const void *selector)
{
	return sizeof(struct IO_Event_Selector_EPoll);
}

static const rb_data_type_t IO_Event_Selector_EPoll_Type = {
	.wrap_struct_name = "IO_Event::Backend::EPoll",
	.function = {
		.dmark = IO_Event_Selector_EPoll_Type_mark,
		.dfree = IO_Event_Selector_EPoll_Type_free,
		.dsize = IO_Event_Selector_EPoll_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

// This represents zero or more fibers waiting for a specific descriptor.
struct IO_Event_Selector_EPoll_Descriptor {
	struct IO_Event_List list;
	
	// The union of all events we are currently waiting on.
	enum IO_Event events;
};

inline static
struct IO_Event_Selector_EPoll_Descriptor * IO_Event_Selector_EPoll_Descriptor_lookup(struct IO_Event_Selector_EPoll *selector, int descriptor)
{
	struct IO_Event_Selector_EPoll_Descriptor *epoll_descriptor = IO_Event_Array_lookup(&selector->descriptors, descriptor);
	
	if (!epoll_descriptor) {
		rb_sys_fail("IO_Event_Selector_EPoll_Descriptor_lookup:IO_Event_Array_lookup");
	}
	
	return epoll_descriptor;
}

void IO_Event_Selector_EPoll_Descriptor_initialize(void *element)
{
	struct IO_Event_Selector_EPoll_Descriptor *epoll_descriptor = element;
	IO_Event_List_initialize(&epoll_descriptor->list);
	epoll_descriptor->events = 0;
}

void IO_Event_Selector_EPoll_Descriptor_free(void *element)
{
	struct IO_Event_Selector_EPoll_Descriptor *epoll_descriptor = element;
	
	IO_Event_List_free(&epoll_descriptor->list);
}

VALUE IO_Event_Selector_EPoll_allocate(VALUE self) {
	struct IO_Event_Selector_EPoll *selector = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	IO_Event_Selector_initialize(&selector->backend, Qnil);
	selector->descriptor = -1;
	selector->blocked = 0;
	
	selector->descriptors.element_initialize = IO_Event_Selector_EPoll_Descriptor_initialize;
	selector->descriptors.element_free = IO_Event_Selector_EPoll_Descriptor_free;
	IO_Event_Array_allocate(&selector->descriptors, 1024, sizeof(struct IO_Event_Selector_EPoll_Descriptor));
	
	return instance;
}

void IO_Event_Interrupt_add(struct IO_Event_Interrupt *interrupt, struct IO_Event_Selector_EPoll *selector) {
	int descriptor = IO_Event_Interrupt_descriptor(interrupt);
	
	struct epoll_event event = {
		.events = EPOLLIN|EPOLLRDHUP|EPOLLONESHOT,
		.data = {.fd = -1},
	};
	
	int result = epoll_ctl(selector->descriptor, EPOLL_CTL_ADD, descriptor, &event);
	
	if (result == -1) {
		rb_sys_fail("IO_Event_Interrupt_add:epoll_ctl");
	}
}

VALUE IO_Event_Selector_EPoll_initialize(VALUE self, VALUE loop) {
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	IO_Event_Selector_initialize(&selector->backend, loop);
	int result = epoll_create1(EPOLL_CLOEXEC);
	
	if (result == -1) {
		rb_sys_fail("IO_Event_Selector_EPoll_initialize:epoll_create");
	} else {
		selector->descriptor = result;
		
		rb_update_max_fd(selector->descriptor);
	}
	
	IO_Event_Interrupt_open(&selector->interrupt);
	IO_Event_Interrupt_add(&selector->interrupt, selector);
	
	return self;
}

VALUE IO_Event_Selector_EPoll_loop(VALUE self) {
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	return selector->backend.loop;
}

VALUE IO_Event_Selector_EPoll_close(VALUE self) {
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	close_internal(selector);
	
	return Qnil;
}

VALUE IO_Event_Selector_EPoll_transfer(VALUE self)
{
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	return IO_Event_Selector_fiber_transfer(selector->backend.loop, 0, NULL);
}

VALUE IO_Event_Selector_EPoll_resume(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	return IO_Event_Selector_resume(&selector->backend, argc, argv);
}

VALUE IO_Event_Selector_EPoll_yield(VALUE self)
{
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	return IO_Event_Selector_yield(&selector->backend);
}

VALUE IO_Event_Selector_EPoll_push(VALUE self, VALUE fiber)
{
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	IO_Event_Selector_queue_push(&selector->backend, fiber);
	
	return Qnil;
}

VALUE IO_Event_Selector_EPoll_raise(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	return IO_Event_Selector_raise(&selector->backend, argc, argv);
}

VALUE IO_Event_Selector_EPoll_ready_p(VALUE self) {
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	return selector->backend.ready ? Qtrue : Qfalse;
}

struct process_wait_arguments {
	struct IO_Event_Selector_EPoll *selector;
	struct IO_Event_Selector_EPoll_Waiting *waiting;
	int pid;
	int descriptor;
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
	
	close(arguments->descriptor);
	
	IO_Event_List_pop(&arguments->waiting->list);
	
	return Qnil;
}

VALUE IO_Event_Selector_EPoll_process_wait(VALUE self, VALUE fiber, VALUE _pid, VALUE _flags) {
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	pid_t pid = NUM2PIDT(_pid);
	// int flags = NUM2INT(_flags);
	
	int descriptor = pidfd_open(pid, 0);
	
	if (descriptor == -1) {
		rb_sys_fail("IO_Event_Selector_EPoll_process_wait:pidfd_open");
	}
	
	rb_update_max_fd(descriptor);
	
	struct IO_Event_Selector_EPoll_Descriptor *epoll_descriptor = IO_Event_Selector_EPoll_Descriptor_lookup(selector, descriptor);
	
	epoll_descriptor->events = IO_EVENT_READABLE;
	
	struct epoll_event event = {
		.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLONESHOT,
		.data = {.fd = descriptor},
	};
	
	int result = epoll_ctl(selector->descriptor, EPOLL_CTL_ADD, descriptor, &event);
	
	if (result == -1) {
		close(descriptor);
		rb_sys_fail("IO_Event_Selector_EPoll_process_wait:epoll_ctl");
	}
	
	struct IO_Event_Selector_EPoll_Waiting waiting = {
		.fiber = fiber,
		.events = IO_EVENT_READABLE,
	};
	
	IO_Event_List_prepend(&epoll_descriptor->list, &waiting.list);
	
	struct process_wait_arguments process_wait_arguments = {
		.selector = selector,
		.pid = pid,
		.descriptor = descriptor,
		.waiting = &waiting,
	};
	
	return rb_ensure(process_wait_transfer, (VALUE)&process_wait_arguments, process_wait_ensure, (VALUE)&process_wait_arguments);
}

static inline
uint32_t epoll_flags_from_events(int events) {
	uint32_t flags = 0;
	
	if (events & IO_EVENT_READABLE) flags |= EPOLLIN;
	if (events & IO_EVENT_PRIORITY) flags |= EPOLLPRI;
	if (events & IO_EVENT_WRITABLE) flags |= EPOLLOUT;
	
	flags |= EPOLLHUP;
	flags |= EPOLLERR;
	
	if (DEBUG) fprintf(stderr, "epoll_flags_from_events events=%d flags=%d\n", events, flags);
	
	return flags;
}

static inline
int events_from_epoll_flags(uint32_t flags) {
	int events = 0;
	
	if (DEBUG) fprintf(stderr, "events_from_epoll_flags flags=%d\n", flags);
	
	// Occasionally, (and noted specifically when dealing with child processes stdout), flags will only be POLLHUP. In this case, we arm the file descriptor for reading so that the HUP will be noted, rather than potentially ignored, since there is no dedicated event for it.
	// if (flags & (EPOLLIN)) events |= IO_EVENT_READABLE;
	if (flags & (EPOLLIN|EPOLLHUP|EPOLLERR)) events |= IO_EVENT_READABLE;
	if (flags & EPOLLPRI) events |= IO_EVENT_PRIORITY;
	if (flags & EPOLLOUT) events |= IO_EVENT_WRITABLE;
	
	return events;
}

struct io_wait_arguments {
	struct IO_Event_Selector_EPoll *selector;
	struct IO_Event_Selector_EPoll_Waiting *waiting;
};

static
VALUE io_wait_ensure(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	IO_Event_List_pop(&arguments->waiting->list);
	
	return Qnil;
};

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	VALUE result = IO_Event_Selector_fiber_transfer(arguments->selector->backend.loop, 0, NULL);
	
	if (DEBUG) fprintf(stderr, "io_wait_transfer errno=%d\n", errno);
	
	// If the fiber is being cancelled, it might be resumed with nil:
	if (!RTEST(result)) {
		if (DEBUG) fprintf(stderr, "io_wait_transfer flags=false\n");
		return Qfalse;
	}
	
	if (DEBUG) fprintf(stderr, "io_wait_transfer flags=%d\n", NUM2INT(result));
	
	return INT2NUM(events_from_epoll_flags(NUM2INT(result)));
};

VALUE IO_Event_Selector_EPoll_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	int descriptor = IO_Event_Selector_io_descriptor(io); 
	struct IO_Event_Selector_EPoll_Descriptor *epoll_descriptor = IO_Event_Selector_EPoll_Descriptor_lookup(selector, descriptor);
	
	struct IO_Event_Selector_EPoll_Waiting waiting = {
		.fiber = fiber,
		.events = NUM2INT(events),
	};
	
	if ((epoll_descriptor->events & waiting.events) != waiting.events) {
		// The descriptor is not already armed for the requested events, so we need to re-arm it:
		struct epoll_event event = {
			.events = epoll_flags_from_events((epoll_descriptor->events | waiting.events)),
			.data = {.fd = descriptor},
		};
		
		int operation = epoll_descriptor->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
		int result = epoll_ctl(selector->descriptor, operation, descriptor, &event);
		
		if (result == -1) {
			if (errno == EPERM) {
				IO_Event_Selector_queue_push(&selector->backend, fiber);
				IO_Event_Selector_yield(&selector->backend);
				return events;
			}
			
			rb_sys_fail("IO_Event_Selector_EPoll_io_wait:epoll_ctl");
		}
		
		epoll_descriptor->events |= waiting.events;
	}
	
	IO_Event_List_prepend(&epoll_descriptor->list, &waiting.list);
	
	struct io_wait_arguments io_wait_arguments = {
		.selector = selector,
		.waiting = &waiting,
	};
	
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
	
	while (true) {
		size_t maximum_size = size - offset;
		ssize_t result = read(arguments->descriptor, (char*)base+offset, maximum_size);
		
		if (result > 0) {
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(errno)) {
			IO_Event_Selector_EPoll_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(IO_EVENT_READABLE));
		} else {
			return rb_fiber_scheduler_io_result(-1, errno);
		}
	}
	
	return rb_fiber_scheduler_io_result(offset, 0);
}

static
VALUE io_read_ensure(VALUE _arguments) {
	struct io_read_arguments *arguments = (struct io_read_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
}

VALUE IO_Event_Selector_EPoll_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length, VALUE _offset) {
	int descriptor = IO_Event_Selector_io_descriptor(io);
	
	size_t offset = NUM2SIZET(_offset);
	size_t length = NUM2SIZET(_length);
	
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

VALUE IO_Event_Selector_EPoll_io_read_compatible(int argc, VALUE *argv, VALUE self)
{
	rb_check_arity(argc, 4, 5);
	
	VALUE _offset = SIZET2NUM(0);
	
	if (argc == 5) {
		_offset = argv[4];
	}
	
	return IO_Event_Selector_EPoll_io_read(self, argv[0], argv[1], argv[2], argv[3], _offset);
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
	
	if (length > size) {
		rb_raise(rb_eRuntimeError, "Length exceeds size of buffer!");
	}
	
	while (true) {
		size_t maximum_size = size - offset;
		ssize_t result = write(arguments->descriptor, (char*)base+offset, maximum_size);
		
		if (result > 0) {
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(errno)) {
			IO_Event_Selector_EPoll_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(IO_EVENT_WRITABLE));
		} else {
			return rb_fiber_scheduler_io_result(-1, errno);
		}
	}
	
	return rb_fiber_scheduler_io_result(offset, 0);
};

static
VALUE io_write_ensure(VALUE _arguments) {
	struct io_write_arguments *arguments = (struct io_write_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
};

VALUE IO_Event_Selector_EPoll_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length, VALUE _offset) {
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

VALUE IO_Event_Selector_EPoll_io_write_compatible(int argc, VALUE *argv, VALUE self)
{
	rb_check_arity(argc, 4, 5);
	
	VALUE _offset = SIZET2NUM(0);
	
	if (argc == 5) {
		_offset = argv[4];
	}
	
	return IO_Event_Selector_EPoll_io_write(self, argv[0], argv[1], argv[2], argv[3], _offset);
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
	struct IO_Event_Selector_EPoll *selector;
	
	int count;
	struct epoll_event events[EPOLL_MAX_EVENTS];

	struct timespec * timeout;
	struct timespec storage;
};

static int make_timeout_ms(struct timespec * timeout) {
	if (timeout == NULL) {
		return -1;
	}
	
	if (timeout_nonblocking(timeout)) {
		return 0;
	}
	
	return (timeout->tv_sec * 1000) + (timeout->tv_nsec / 1000000);
}

static
int enosys_error(int result) {
	if (result == -1) {
		return errno == ENOSYS;
	}
	
	return 0;
}

static
void * select_internal(void *_arguments) {
	struct select_arguments * arguments = (struct select_arguments *)_arguments;
	
#if defined(HAVE_EPOLL_PWAIT2)
	arguments->count = epoll_pwait2(arguments->selector->descriptor, arguments->events, EPOLL_MAX_EVENTS, arguments->timeout, NULL);
	
	// Comment out the above line and enable the below lines to test ENOSYS code path.
	// arguments->count = -1;
	// errno = ENOSYS;
	
	if (!enosys_error(arguments->count)) {
		return NULL;
	}
	else {
		// Fall through and execute epoll_wait fallback.
	}
#endif
	
	arguments->count = epoll_wait(arguments->selector->descriptor, arguments->events, EPOLL_MAX_EVENTS, make_timeout_ms(arguments->timeout));
	
	return NULL;
}

static
void select_internal_without_gvl(struct select_arguments *arguments) {
	arguments->selector->blocked = 1;
	rb_thread_call_without_gvl(select_internal, (void *)arguments, RUBY_UBF_IO, 0);
	arguments->selector->blocked = 0;
	
	if (arguments->count == -1) {
		if (errno != EINTR) {
			rb_sys_fail("select_internal_without_gvl:epoll_wait");
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
			rb_sys_fail("select_internal_with_gvl:epoll_wait");
		} else {
			arguments->count = 0;
		}
	}
}

static
void IO_Event_Selector_EPoll_handle(struct IO_Event_Selector_EPoll *selector, const struct epoll_event *event)
{
	int descriptor = event->data.fd;
	
	// This is the mask of all events that occured for the given descriptor:
	enum IO_Event io_event = events_from_epoll_flags(event->events);
	
	// This is the mask of all events that we could process:
	enum IO_Event matched_events = 0;
	
	struct IO_Event_Selector_EPoll_Descriptor *epoll_descriptor = IO_Event_Selector_EPoll_Descriptor_lookup(selector, descriptor);
	struct IO_Event_List *list = &epoll_descriptor->list;
	struct IO_Event_List *node = list->tail;
	struct IO_Event_List saved = {NULL, NULL};
	
	// It's possible (but unlikely) that the address of list will changing during iteration.
	while (node != list) {
		struct IO_Event_Selector_EPoll_Waiting *waiting = (struct IO_Event_Selector_EPoll_Waiting *)node;
		
		enum IO_Event matching_events = waiting->events & io_event;
		
		if (DEBUG) fprintf(stderr, "IO_Event_Selector_EPoll_handle: descriptor=%d, events=%d, matching_events=%d\n", descriptor, io_event, matching_events);
		
		if (matching_events) {
			matched_events |= matching_events;
			
			IO_Event_List_append(node, &saved);
			
			VALUE argument = RB_INT2NUM(matching_events);
			IO_Event_Selector_fiber_transfer(waiting->fiber, 1, &argument);
			
			node = saved.tail;
			IO_Event_List_pop(&saved);
		} else {
			node = node->tail;
		}
	}
	
	// ... if we receive events we are not waiting for, we should disable them:
	if (io_event != matched_events) {
		struct epoll_event epoll_event = {
			.events = epoll_flags_from_events(epoll_descriptor->events),
			.data = {.fd = descriptor}
		};
	
		if (epoll_descriptor->events) {
			epoll_ctl(selector->descriptor, EPOLL_CTL_MOD, descriptor, &epoll_event);
		} else {
			epoll_ctl(selector->descriptor, EPOLL_CTL_DEL, descriptor, &epoll_event);
		}
	}
}

// TODO This function is not re-entrant and we should document and assert as such.
VALUE IO_Event_Selector_EPoll_select(VALUE self, VALUE duration) {
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	int ready = IO_Event_Selector_queue_flush(&selector->backend);
	
	struct select_arguments arguments = {
		.selector = selector,
		.storage = {
			.tv_sec = 0,
			.tv_nsec = 0
		},
	};

	arguments.timeout = &arguments.storage;

	// Process any currently pending events:
	select_internal_with_gvl(&arguments);
	
	// If we:
	// 1. Didn't process any ready fibers, and
	// 2. Didn't process any events from non-blocking select (above), and
	// 3. There are no items in the ready list,
	// then we can perform a blocking select.
	if (!ready && !arguments.count && !selector->backend.ready) {
		arguments.timeout = make_timeout(duration, &arguments.storage);
		
		if (!timeout_nonblocking(arguments.timeout)) {
			// Wait for events to occur
			select_internal_without_gvl(&arguments);
		}
	}
	
	for (int i = 0; i < arguments.count; i += 1) {
		const struct epoll_event *event = &arguments.events[i];
		if (DEBUG) fprintf(stderr, "-> ptr=%p events=%d\n", event->data.ptr, event->events);
		
		if (event->data.fd >= 0) {
			IO_Event_Selector_EPoll_handle(selector, event);
		} else {
			IO_Event_Interrupt_clear(&selector->interrupt);
		}
	}
	
	return INT2NUM(arguments.count);
}

VALUE IO_Event_Selector_EPoll_wakeup(VALUE self) {
	struct IO_Event_Selector_EPoll *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, selector);
	
	// If we are blocking, we can schedule a nop event to wake up the selector:
	if (selector->blocked) {
		IO_Event_Interrupt_signal(&selector->interrupt);
		
		return Qtrue;
	}
	
	return Qfalse;
}

void Init_IO_Event_Selector_EPoll(VALUE IO_Event_Selector) {
	IO_Event_Selector_EPoll = rb_define_class_under(IO_Event_Selector, "EPoll", rb_cObject);
	rb_gc_register_mark_object(IO_Event_Selector_EPoll);
	
	rb_define_alloc_func(IO_Event_Selector_EPoll, IO_Event_Selector_EPoll_allocate);
	rb_define_method(IO_Event_Selector_EPoll, "initialize", IO_Event_Selector_EPoll_initialize, 1);
	
	rb_define_method(IO_Event_Selector_EPoll, "loop", IO_Event_Selector_EPoll_loop, 0);
	
	rb_define_method(IO_Event_Selector_EPoll, "transfer", IO_Event_Selector_EPoll_transfer, 0);
	rb_define_method(IO_Event_Selector_EPoll, "resume", IO_Event_Selector_EPoll_resume, -1);
	rb_define_method(IO_Event_Selector_EPoll, "yield", IO_Event_Selector_EPoll_yield, 0);
	rb_define_method(IO_Event_Selector_EPoll, "push", IO_Event_Selector_EPoll_push, 1);
	rb_define_method(IO_Event_Selector_EPoll, "raise", IO_Event_Selector_EPoll_raise, -1);
	
	rb_define_method(IO_Event_Selector_EPoll, "ready?", IO_Event_Selector_EPoll_ready_p, 0);
	
	rb_define_method(IO_Event_Selector_EPoll, "select", IO_Event_Selector_EPoll_select, 1);
	rb_define_method(IO_Event_Selector_EPoll, "wakeup", IO_Event_Selector_EPoll_wakeup, 0);
	rb_define_method(IO_Event_Selector_EPoll, "close", IO_Event_Selector_EPoll_close, 0);
	
	rb_define_method(IO_Event_Selector_EPoll, "io_wait", IO_Event_Selector_EPoll_io_wait, 3);
	
#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(IO_Event_Selector_EPoll, "io_read", IO_Event_Selector_EPoll_io_read_compatible, -1);
	rb_define_method(IO_Event_Selector_EPoll, "io_write", IO_Event_Selector_EPoll_io_write_compatible, -1);
#endif
	
	// Once compatibility isn't a concern, we can do this:
	// rb_define_method(IO_Event_Selector_EPoll, "io_read", IO_Event_Selector_EPoll_io_read, 5);
	// rb_define_method(IO_Event_Selector_EPoll, "io_write", IO_Event_Selector_EPoll_io_write, 5);
	
	rb_define_method(IO_Event_Selector_EPoll, "process_wait", IO_Event_Selector_EPoll_process_wait, 3);
}
