// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    schedule.cpp

    Core device execution and scheduling engine.

---

	Still to do:
	- Fix remaining drivers that are buggy
	- Clean up timer devices
	- Clean up more timers in devices/drivers

***************************************************************************/

#include "emu.h"
#include "emuopts.h"
#include "debugger.h"
#include "hashing.h"


//**************************************************************************
//  DEBUGGING
//**************************************************************************

#define VERBOSE 1

#define LOG(...)  do { if (VERBOSE) machine().logerror(__VA_ARGS__); } while (0)
#define PRECISION 18



//**************************************************************************
//  EMU TIMER CB
//**************************************************************************

//-------------------------------------------------
//  timer_callback - constructor
//-------------------------------------------------

timer_callback::timer_callback(persistent_timer *persistent) :
	m_ptr(nullptr),
	m_scheduler(nullptr),
	m_next_registered(nullptr),
	m_persistent(persistent),
	m_device(nullptr),
	m_unique_hash(0),
	m_save_index(0)
{
}


//-------------------------------------------------
//  ~timer_callback - destructor
//-------------------------------------------------

timer_callback::~timer_callback()
{
	if (m_scheduler != nullptr)
		m_scheduler->deregister_callback(*this);
}


//-------------------------------------------------
//  timer_callback - copy constructor
//-------------------------------------------------

timer_callback::timer_callback(timer_callback const &src) :
	m_delegate(src.m_delegate),
	m_ptr(src.m_ptr),
	m_scheduler(src.m_scheduler),
	m_next_registered(src.m_next_registered),
	m_persistent(nullptr),
	m_device(src.m_device),
	m_unique_hash(src.m_unique_hash),
	m_save_index(src.m_save_index),
	m_unique_id(src.m_unique_id)
{
}


//-------------------------------------------------
//  operator= - copy assignment
//-------------------------------------------------

timer_callback &timer_callback::operator=(timer_callback const &src)
{
	if (&src != this)
	{
		m_delegate = src.m_delegate;
		m_ptr = src.m_ptr;
		m_device = src.m_device;
		m_scheduler = src.m_scheduler;
		m_next_registered = src.m_next_registered;
		// deliberately do not touch m_persistent since it is
		// an allocation-only property
		m_unique_hash = src.m_unique_hash;
		m_unique_id = src.m_unique_id;
	}
	return *this;
}


//-------------------------------------------------
//  set_ptr - set the callback's pointer value;
//  only valid at initialization
//-------------------------------------------------

timer_callback &timer_callback::set_ptr(void *ptr)
{
	// only allowed to set pointers prior to execution; use the save state
	// registration_allowed() as a proxy for this
	if (m_scheduler != nullptr && !m_scheduler->machine().save().registration_allowed())
		throw emu_fatalerror("Timer pointers must remain constant after creation.");
	m_ptr = ptr;
	return *this;
}


//-------------------------------------------------
//  set_device - set the callback's associated
//  device; only valid at initialization
//-------------------------------------------------

timer_callback &timer_callback::set_device(device_t &device)
{
	// only allowed to set pointers prior to execution; use the save state
	// registration_allowed() as a proxy for this
	if (!m_scheduler->machine().save().registration_allowed())
		throw emu_fatalerror("Timer devices must remain constant after creation.");
	m_device = &device;
	return *this;
}


//-------------------------------------------------
//  init_base - register a callback
//-------------------------------------------------

timer_callback &timer_callback::init_base(device_scheduler *scheduler, timer_expired_delegate const &delegate, char const *unique, const char *unique2)
{
	// build the full name, appending the unique identifier(s) if present
	std::string fullid = delegate.name();
	if (unique != nullptr)
	{
		fullid += "/";
		fullid += unique;
	}
	if (unique2 != nullptr)
	{
		fullid += "/";
		fullid += unique2;
	}

	// if not already registered, just pass through
	if (m_next_registered == nullptr)
	{
		m_delegate = delegate;
		m_scheduler = scheduler;
		m_unique_id = fullid;
		m_unique_hash = util::crc32_creator::simple(fullid.c_str(), fullid.length());
		m_save_index = m_scheduler->register_callback(*this);
	}

	// otherwise, make sure we match
	else
	{
		if (m_delegate != delegate)
			throw emu_fatalerror("timer_callback::init called multiple times on the same object with different callbacks.");
		if (m_unique_id != fullid)
			throw emu_fatalerror("timer_callback::init called multiple times on the same object with different ids (%s vs. %s).", m_unique_id.c_str(), fullid.c_str());
	}
	return *this;
}


//-------------------------------------------------
//  init_device - register this callback,
//  associated with a device
//-------------------------------------------------

timer_callback &timer_callback::init_device(device_t &device, timer_expired_delegate const &delegate, char const *unique)
{
	return init_base(device.has_running_machine() ? &device.machine().scheduler() : nullptr, delegate, device.tag(), unique);
}


//-------------------------------------------------
//  init_clone - initialize as a clone of another
//  callback, but with a different delegate
//-------------------------------------------------

timer_callback &timer_callback::init_clone(timer_callback const &src, timer_expired_delegate const &delegate)
{
	// start with a direct copy
	*this = src;

	// replace the delegate and clear the registration
	m_delegate = delegate;
	m_next_registered = nullptr;
	return *this;
}



//**************************************************************************
//  TIMER INSTANCE
//**************************************************************************

//-------------------------------------------------
//  timer_instance - constructor
//-------------------------------------------------

timer_instance::timer_instance() :
	m_next(nullptr),
	m_prev(nullptr),
	m_start(attotime::zero),
	m_expire(attotime::never),
	m_callback(nullptr),
	m_param{ 0, 0, 0 },
	m_active(false)
{
}


//-------------------------------------------------
//  ~timer_instance - destructor
//-------------------------------------------------

timer_instance::~timer_instance()
{
}


//-------------------------------------------------
//  init_persistent - initialize a persistent
//  system or device timer; persistent timers can
//  be saved and start off in a disabled state
//-------------------------------------------------

timer_instance &timer_instance::init_persistent(timer_callback &callback)
{
	scheduler_assert(callback.persistent() != nullptr);
	m_callback = &callback;

	// everything else has been initialized by the constructor;
	// unlike transient timers, we are embedded in the persistent_timer
	// object, and so don't need to worry about re-use

	return *this;
}


//-------------------------------------------------
//  init_transient - initialize a transient
//  system timer; transient timers have a parameter
//  and expiration time from the outset
//-------------------------------------------------

timer_instance &timer_instance::init_transient(timer_callback &callback, attotime const &duration, bool absolute)
{
	scheduler_assert(callback.persistent() == nullptr);
	m_callback = &callback;

	// ensure the entire timer state is clean, since we re-use these
	// instances for fast allocation
	m_param[0] = m_param[1] = m_param[2] = 0;
	m_active = false;

	// add immediately to the active queue
	attotime start = callback.scheduler().time();
	return insert(start, absolute ? duration : (start + duration));
}


//-------------------------------------------------
//  elapsed - return the amount of time since the
//  timer was started
//-------------------------------------------------

attotime timer_instance::elapsed() const noexcept
{
	return scheduler().time() - m_start;
}


//-------------------------------------------------
//  remaining - return the amount of time
//  remaining until the timer expires
//-------------------------------------------------

attotime timer_instance::remaining() const noexcept
{
	attotime curtime = scheduler().time();
	if (curtime >= m_expire)
		return attotime::zero;
	return m_expire - curtime;
}


//-------------------------------------------------
//  save - save our state to the given save data
//  structure
//-------------------------------------------------

timer_instance &timer_instance::save(timer_instance_save &dst)
{
	dst.start = m_start;
	dst.expire = m_expire;
	dst.param[0] = m_param[0];
	dst.param[1] = m_param[1];
	dst.param[2] = m_param[2];
	dst.hash = m_callback->unique_hash();
	dst.save_index = m_callback->save_index();
	return *this;
}


//-------------------------------------------------
//  restore - restore our state from the given
//  save data structure
//-------------------------------------------------

timer_instance &timer_instance::restore(timer_instance_save const &src, timer_callback &callback)
{
	m_callback = &callback;
	m_param[0] = src.param[0];
	m_param[1] = src.param[1];
	m_param[2] = src.param[2];
	m_active = false;
	m_start = src.start;
	m_expire = src.expire;
	return insert(src.start, src.expire);
}


//-------------------------------------------------
//  insert - insert us into the scheduler's
//  active timer queue
//-------------------------------------------------

timer_instance &timer_instance::insert(attotime const &start, attotime const &expire)
{
	m_start = start;
	m_expire = expire;
	m_active = true;
	return m_callback->scheduler().instance_insert(*this);
}


//-------------------------------------------------
//  remove - remove us from the scheduler's
//  active timer queue
//-------------------------------------------------

timer_instance &timer_instance::remove()
{
	m_active = false;
	return m_callback->scheduler().instance_remove(*this);
}


//-------------------------------------------------
//  dump - dump internal state to a single output
//  line in the error log
//-------------------------------------------------

void timer_instance::dump() const
{
	persistent_timer *persistent = m_callback->persistent();
	osd_printf_info("%p: %s exp=%15s start=%15s ptr=%p param=%p/%p/%p",
		this,
		(m_callback->persistent() != nullptr) ? "P" : "T",
		m_expire.as_string(PRECISION),
		m_start.as_string(PRECISION),
		m_callback->ptr(),
		(void *)m_param[0], (void *)m_param[1], (void *)m_param[2]);

	if (persistent != nullptr && persistent->periodic())
		osd_printf_info(" per=%15s", persistent->period().as_string(PRECISION));

	if (is_device_timer())
		osd_printf_info(" dev=%s id=%d\n", m_callback->device()->tag(), id());
	else if (persistent != nullptr && persistent->periodic())
		osd_printf_info(" cb=%s\n", persistent->callback().name());
	else
		osd_printf_info(" cb=%s\n", m_callback->name());
}



//**************************************************************************
//  PERSISTENT_TIMER
//**************************************************************************

//-------------------------------------------------
//  persistent_timer - constructor
//-------------------------------------------------

persistent_timer::persistent_timer() :
	m_modified(false),
	m_callback(this),
	m_periodic_callback(this)
{
}


//-------------------------------------------------
//  ~persistent_timer - destructor
//-------------------------------------------------

persistent_timer::~persistent_timer()
{
}


//-------------------------------------------------
//  enable - enable a timer, returning the
//  previous state
//-------------------------------------------------

bool persistent_timer::enable(bool enable)
{
	// fetch the previous state and set the new one
	bool old = enabled();
	m_enabled = enable;

	// if nothing changed, leave it alone
	if (old == enable)
		return old;

	// remove if previously active
	if (m_instance.active())
		m_instance.remove();

	// only re-insert if enabled
	if (enable)
		m_instance.insert(m_instance.start(), m_instance.expire());

	// mark as modified
	m_modified = true;
	return old;
}


//-------------------------------------------------
//  adjust_internal - change the timer's start time,
//  parameter, or period
//-------------------------------------------------

persistent_timer &persistent_timer::adjust_internal(attotime const &delay, s32 param, attotime const &period, bool absolute)
{
	// set the parameters first
	m_instance.set_param(param);

	// adjust implicitly enables the timer
	m_enabled = true;

	// set the period and adjust the callback appropriately
	m_period = period.is_zero() ? attotime::never : period;
	if (periodic())
		m_instance.m_callback = &m_periodic_callback;
	else
		m_instance.m_callback = &m_callback;

	// compute start/expire times
	attotime start = m_callback.scheduler().time();
	attotime expire;
	if (absolute)
		expire = delay;
	else if (delay.seconds() >= 0)
		expire = start + delay;

	// then insert into the active list, removing first if previously active
	if (m_instance.active())
		m_instance.remove();
	if (!expire.is_never())
		m_instance.insert(start, expire);
	else
	{
		m_instance.m_start = start;
		m_instance.m_expire = expire;
	}

	// mark as modified
	m_modified = true;
	return *this;
}


//-------------------------------------------------
//  init_common - handle common initialization
//  tasks
//-------------------------------------------------

persistent_timer &persistent_timer::init_common()
{
	// initialize the timer instance
	m_instance.init_persistent(m_callback);

	// create the periodic callback by cloning (but not registering) our periodic
	// front-end callback
	m_periodic_callback.init_clone(m_callback, timer_expired_delegate(FUNC(persistent_timer::periodic_callback), this));

	return *this;
}


//-------------------------------------------------
//  periodic_callback - callback to handle
//  periodic timers
//-------------------------------------------------

void persistent_timer::periodic_callback(timer_instance const &timer)
{
	// clear the modified state
	m_modified = false;

	// call the real callback
	m_callback(timer);

	// if the timer wasn't modified during the callback, advance by one period
	if (!m_modified)
		m_instance.insert(m_instance.m_expire, m_instance.m_expire + m_period);
}



//**************************************************************************
//  TRANSIENT TIMER FACTORY
//**************************************************************************

//-------------------------------------------------
//  transient_timer_factory - constructor
//-------------------------------------------------

transient_timer_factory::transient_timer_factory()
{
}



//**************************************************************************
//  DEVICE SCHEDULER
//**************************************************************************

//-------------------------------------------------
//  device_scheduler - constructor
//-------------------------------------------------

device_scheduler::device_scheduler(running_machine &machine) :
	m_machine(machine),
	m_executing_device(nullptr),
	m_execute_list(nullptr),
	m_active_timers_head(&m_active_timers_tail),
	m_free_timers(nullptr),
	m_registered_callbacks(nullptr),
	m_callback_timer(nullptr),
	m_callback_timer_expire_time(attotime::zero),
	m_suspend_changes_pending(true),
	m_in_timeslice(false),
	m_hard_stopping(false),
	m_load_after_stop(false),
	m_quantum_minimum(subseconds::from_nsec(1) / 1000),
	m_quantum_count(0),
	m_midslice_restore(false),
	m_save_executing(-1),
	m_save_icount(0),
	m_save_target(subseconds::zero())
{
	// create a factory for empty timers
	m_empty_timer.init(*this, *this, FUNC(device_scheduler::empty_timer));

	// create a factory for trigger timers
	m_timed_trigger.init(*this, *this, FUNC(device_scheduler::timed_trigger));

	// register for presave and postload early so we get called first
	machine.save().register_presave(save_prepost_delegate(FUNC(device_scheduler::presave), this));
	machine.save().register_postload(save_prepost_delegate(FUNC(device_scheduler::postload), this));
}


//-------------------------------------------------
//  device_scheduler - destructor
//-------------------------------------------------

device_scheduler::~device_scheduler()
{
#if (COLLECT_SCHEDULER_STATS)
	double seconds = m_basetime.absolute().as_double();
	if (seconds > 0)
	{
		osd_printf_info("%12.2f timeslice\n", m_timeslice / seconds);
		osd_printf_info("%12.2f    avg reps until minslice\n", (1.0 * m_timeslice_inner1) / m_timeslice);
		osd_printf_info("%12.2f    avg reps through execution loop until timer\n", (1.0 * m_timeslice_inner2) / m_timeslice_inner1);
		osd_printf_info("%12.2f    avg run_for called per execution loop\n", (1.0 * m_timeslice_inner3) / m_timeslice_inner2);
		osd_printf_info("%12.2f execute_timers: (%.2f avg)\n", m_execute_timers / seconds, 1.0 * m_execute_timers_average / m_execute_timers);
		osd_printf_info("%12.2f    update_basetime\n", m_update_basetime / seconds);
		osd_printf_info("%12.2f    apply_suspend_changes\n", m_apply_suspend_changes / seconds);
		osd_printf_info("%12.2f compute_perfect_interleave\n", m_compute_perfect_interleave / seconds);
		osd_printf_info("%12.2f add_scheduling_quantum\n", m_add_scheduling_quantum / seconds);
		osd_printf_info("%12.2f instance_alloc (%u full)\n", m_instance_alloc / seconds, u32(m_instance_alloc_full));
		u64 total_insert = m_instance_insert_head + m_instance_insert_tail + m_instance_insert_middle;
		osd_printf_info("%12.2f instance_insert:\n", total_insert / seconds);
		osd_printf_info("%12.2f    head (%.2f%%)\n", m_instance_insert_head / seconds, (100.0 * m_instance_insert_head) / total_insert);
		osd_printf_info("%12.2f    tail (%.2f%%)\n", m_instance_insert_tail / seconds, (100.0 * m_instance_insert_tail) / total_insert);
		osd_printf_info("%12.2f    middle (%.2f%%, avg search = %.2f)\n", m_instance_insert_middle / seconds, (100.0 * m_instance_insert_middle) / total_insert, 1.0 * m_instance_insert_average / m_instance_insert_middle);
		osd_printf_info("%12.2f instance_remove\n", m_instance_remove / seconds);
		osd_printf_info("%12.2f empty_timer\n", m_empty_timer_calls / seconds);
		osd_printf_info("%12.2f timed_trigger\n", m_timed_trigger_calls / seconds);
		osd_printf_info("\ntimers:\n");

		using cbptr = timer_callback *;
		std::vector<cbptr> timers;
		for (timer_callback *cb = m_registered_callbacks; cb != nullptr; cb = cb->m_next_registered)
			if (cb->m_calls != 0)
				timers.push_back(cb);
		std::sort(timers.begin(), timers.end(),
			[](cbptr const &a, cbptr const &b)
			{
				return (a->m_calls > b->m_calls);
			});

		static char const *forms[] = { "native", " void ", " int  ", "legacy", "intint", " int3 " };
		for (auto &cb : timers)
		{
			char const *form = forms[cb->m_delegate.m_form];
			if (cb->device() != nullptr)
				form = "device";
			osd_printf_info("%12.2f %s %s\n", cb->m_calls / seconds, form, cb->m_unique_id.c_str());
		}
	}
#endif
}


//-------------------------------------------------
//  finalize - finalize setup once all the devices
//  have been started
//-------------------------------------------------

void device_scheduler::finalize()
{
	// set the core scheduling quantum, ensuring it's no longer than the maximum
	subseconds min_quantum = machine().config().maximum_quantum(MAX_QUANTUM).as_subseconds();
	min_quantum = std::min(min_quantum, MAX_QUANTUM);

	// create a fast list of executing devices
	rebuild_execute_list();

	// if the configuration specifies a device to make perfect, pick that as the minimum
	device_execute_interface *const exec(machine().config().perfect_quantum_device());
	if (exec != nullptr)
		min_quantum = std::min(exec->minimum_quantum(), min_quantum);

	// use this as our baseline quantum
	add_scheduling_quantum(min_quantum, attotime::never);

	// build the initial list of executing devices and compute the perfect interleave
	compute_perfect_interleave();

	// save states
	register_save(machine().save());
}


//-------------------------------------------------
//  hard_stop - aggressively force an exit
//-------------------------------------------------

void device_scheduler::hard_stop(bool load_after_stop)
{
	// doesn't matter if we're not within a timeslice
	if (!m_in_timeslice)
	{
		if (load_after_stop)
			return machine().immediate_load();
		return;
	}

	// set the flag for some early outs
	m_hard_stopping = true;
	m_load_after_stop = load_after_stop;

	// set the first timer expiration to immediate; this saves us checking
	// the flag in the time-sensitive innermost loop
	m_first_timer_expire.set(time());

	// if currently executing, go even further to get us out ASAP
	if (m_executing_device != nullptr)
	{
		// force the icount to 0 immediately so it will exit its loop
		*m_executing_device->m_icountptr = 0;

		// also set the nextexec pointer to null so we don't execute other
		// devices
		m_executing_device->m_nextexec = nullptr;
	}
}


//-------------------------------------------------
//  dump_timers - dump the current timer state
//-------------------------------------------------

void device_scheduler::dump_timers() const
{
	osd_printf_info("=============================================\n");
	osd_printf_info("Timer Dump: Time = %15s\n", time().as_string(PRECISION));
	for (timer_instance *timer = m_active_timers_head; timer != &m_active_timers_tail; timer = timer->next())
		timer->dump();
	osd_printf_info("=============================================\n");
}


//-------------------------------------------------
//  register_save - register all save states
//-------------------------------------------------

void device_scheduler::register_save(save_manager &save)
{
	// register global states
	save.save_item(NAME(m_basetime.m_relative));
	save.save_item(NAME(m_basetime.m_absolute));
	save.save_item(NAME(m_basetime.m_base));

	// register all initialized persistent timers
	int index = 0;
	for (timer_callback *cb = m_registered_callbacks; cb != nullptr; cb = cb->m_next_registered)
		if (cb->persistent() != nullptr)
			cb->persistent()->register_save(save, index++);

	// and register our save area for transient timers
	for (int index = 0; index < TIMER_SAVE_SLOTS; index++)
		m_timer_save[index].register_save(save, index);

	// save executing state
	save.save_item(NAME(m_save_executing));
	save.save_item(NAME(m_save_icount));
	save.save_item(NAME(m_save_target));

	// and register the quantum slots
	save.save_item(NAME(m_quantum_count));
	for (int index = 0; index < MAX_ACTIVE_QUANTA; index++)
		m_quantum_slot[index].register_save(save, index);
}


//-------------------------------------------------
//  time - return the current time
//-------------------------------------------------

attotime device_scheduler::time() const noexcept
{
	// if we're currently in a callback, use the timer's expiration time as a base
	if (m_callback_timer != nullptr)
		return m_callback_timer_expire_time;

	// if we're executing as a particular CPU, use its local time as a base
	// otherwise, return the global base time
	return (m_executing_device != nullptr) ? m_executing_device->local_time() : m_basetime.absolute_no_update();
}


//-------------------------------------------------
//  abort_timeslice - abort execution for the
//  current timeslice
//-------------------------------------------------

void device_scheduler::abort_timeslice()
{
	if (m_executing_device != nullptr)
		m_executing_device->abort_timeslice();
}


//-------------------------------------------------
//  trigger - generate a global trigger
//-------------------------------------------------

void device_scheduler::trigger(int trigid, attotime const &after)
{
	// if we have a non-zero time, schedule a timer
	if (after != attotime::zero)
		m_timed_trigger.call_after(after, trigid);

	// send the trigger to everyone who cares
	else
		for (device_execute_interface *exec = m_execute_list; exec != nullptr; exec = exec->m_nextexec)
			exec->trigger(trigid);
}


//-------------------------------------------------
//  register_callback - register a timer
//  expired callback
//-------------------------------------------------

u32 device_scheduler::register_callback(timer_callback &callback)
{
	// look for duplicates and compute a unique id
	u32 index = 0;
	for (timer_callback *scan = m_registered_callbacks; scan != nullptr; scan = scan->m_next_registered)
		if (scan->unique_hash() == callback.unique_hash())
			index++;

	// now hook us in
	callback.m_next_registered = m_registered_callbacks;
	m_registered_callbacks = &callback;
	return index;
}


//-------------------------------------------------
//  deregister_callback - deregister a timer
//  expired callback
//-------------------------------------------------

void device_scheduler::deregister_callback(timer_callback &callback)
{
	for (timer_callback **nextptr = &m_registered_callbacks; *nextptr != nullptr; nextptr = &(*nextptr)->m_next_registered)
		if (*nextptr == &callback)
		{
			*nextptr = callback.m_next_registered;
			return;
		}
}


//-------------------------------------------------
//  presave - before creating a save state
//-------------------------------------------------

void device_scheduler::presave()
{
	// report the timer state after a log
	LOG("Prior to saving state:\n");
//#if VERBOSE
	dump_timers();
//#endif

	// if we're in the middle of a timeslice, save the info
	if (m_executing_device != nullptr)
	{
		m_save_executing = 0;
		for (device_execute_interface *exec = m_execute_list; exec != m_executing_device; exec = exec->m_nextexec)
			m_save_executing++;
		m_save_icount = *m_executing_device->m_icountptr;
	}
	else
	{
		m_save_executing = -1;
		m_save_icount = 0;
		m_save_target = subseconds::zero();
	}

	// copy in all the timer instance data to the save area
	int index = 0;
	for (timer_instance *timer = m_active_timers_head; timer != &m_active_timers_tail && index < TIMER_SAVE_SLOTS; timer = timer->next())
		timer->save(m_timer_save[index++]);

	// warn if we ran out of space
	if (index == TIMER_SAVE_SLOTS)
		osd_printf_warning("Not enough slots to save all active timers; state may not restore properly.");

	// zero out the remainder
	for ( ; index < TIMER_SAVE_SLOTS; index++)
	{
		auto &dest = m_timer_save[index];
		dest.start = attotime::zero;
		dest.expire = attotime::never;
		dest.param[0] = dest.param[1] = dest.param[2] = 0;
		dest.hash = 0;
		dest.save_index = 0;
	}
}


//-------------------------------------------------
//  postload - after loading a save state
//-------------------------------------------------

void device_scheduler::postload()
{
	// empty the list of any active timer instances
	while (m_active_timers_head != &m_active_timers_tail)
	{
		auto &prevhead = *m_active_timers_head;
		m_active_timers_head = prevhead.m_next;
		prevhead.m_active = false;
		instance_reclaim(prevhead);
	}

	// now go through the restored save area and recreate all the timers
	for (int index = 0; index < TIMER_SAVE_SLOTS; index++)
	{
		// scan until we find a never-expiring timer
		auto &dest = m_timer_save[index];
		if (dest.expire.is_never())
			break;

		// find a matching callback
		timer_callback *cb;
		for (cb = m_registered_callbacks; cb != nullptr; cb = cb->m_next_registered)
			if (cb->unique_hash() == dest.hash && cb->save_index() == dest.save_index)
				break;

		// if we can't find the timer, that's a concern (probably fatal)
		if (cb == nullptr)
		{
			osd_printf_warning("Unable to find matching callback for %08X\n", dest.hash);
			continue;
		}

		// restore appropriately
		if (cb->persistent() != nullptr)
		{
			persistent_timer &timer = *cb->persistent();
			timer.m_instance.restore(dest, timer.periodic() ? timer.m_periodic_callback : timer.m_callback);
		}
		else
			instance_alloc().restore(dest, *cb);
	}

	// force a refresh of things that are lazily updated
	update_basetime();
	update_first_timer_expire();
	m_suspend_changes_pending = true;

	// if we're restoring mid-timeslice, flag it
	m_midslice_restore = (m_save_executing != -1);

	// report the timer state after a log
	LOG("After resetting/reordering timers:\n");
//#if VERBOSE
	dump_timers();
//#endif
}


//-------------------------------------------------
//  timeslice_core - execute all devices for a
//  single timeslice
//-------------------------------------------------

void device_scheduler::timeslice_core(subseconds minslice)
{
	// if we're in the middle of restoring from a timeslice, handle the
	// first timeslice as a partial special case
	if (m_midslice_restore)
	{
		timeslice_partial();
		m_midslice_restore = false;
		if (m_hard_stopping)
			return hard_stop_complete();
	}

	// officially within a timeslice now; clear any hard stopping state
	m_in_timeslice = true;
	m_hard_stopping = false;

	INCREMENT_SCHEDULER_STAT(m_timeslice);

	// subseconds counts up to 2 seconds; once we pass 1 second, reset
	if (m_basetime.relative() >= subseconds::one_second())
		update_basetime();

	// run at least the given number of subseconds
	subseconds basetime = m_basetime.relative();
	subseconds endslice = basetime + minslice;
	do
	{
		INCREMENT_SCHEDULER_STAT(m_timeslice_inner1);

		LOG("------------------\n");

		// if the current quantum has expired, find a new one
		while (basetime >= m_quantum_slot[0].m_expire.relative())
		{
			m_quantum_count--;
			memmove(&m_quantum_slot[0], &m_quantum_slot[1], m_quantum_count * sizeof(m_quantum_slot[0]));
		}

		// loop until we hit the next timer
		while (basetime < m_first_timer_expire.relative())
		{
			INCREMENT_SCHEDULER_STAT(m_timeslice_inner2);

			// by default, assume our target is the end of the next quantum
			subseconds target = basetime + m_quantum_slot[0].m_actual;

			// however, if the next timer is going to fire before then, override
			if (m_first_timer_expire.relative() < target)
				target = m_first_timer_expire.relative();
			m_save_target = target;

			LOG("timeslice: target = %sas\n", attotime(target).as_string(18));

			// do we have pending suspension changes?
			if (m_suspend_changes_pending)
				apply_suspend_changes(true);

			// loop over all executing devices
			for (device_execute_interface *exec = m_execute_list ; exec != nullptr; exec = exec->m_nextexec)
			{
				// compute how many subseconds to execute this device
				subseconds delta = target - exec->m_localtime.relative() - subseconds::unit();

				// if we're already ahead, do nothing; in theory we should do this 0 as
				// well, but it's a rare case and the compiler tends to be able to
				// optimize a strict < 0 check better than <= 0
				if (delta < subseconds::zero())
					continue;

				INCREMENT_SCHEDULER_STAT(m_timeslice_inner3);

#if VERBOSE
				u64 start_cycles = exec->total_cycles();
				LOG("  %12.12s: t=%018I64das %018I64das = %dc; ", exec->device().tag(), exec->m_localtime.relative().raw(), delta.raw(), delta / exec->m_subseconds_per_cycle + 1);
#endif

				// store a pointer to the executing device so that we know the
				// relevant active context
				m_executing_device = exec;

				// execute for the given number of subseconds
				subseconds localtime = exec->run_for(delta);

#if VERBOSE
				m_executing_device = nullptr;
				u64 end_cycles = exec->total_cycles();
				LOG(" ran %dc, %1I64dc total", s32(end_cycles - start_cycles), end_cycles);
#endif

				// if the new local device time is less than our target, move the
				// target up, but not before the base
				if (UNEXPECTED(localtime < target))
				{
					m_save_target = target = std::max(localtime, basetime);
					LOG(" (new target)");
				}
				LOG("\n");
			}

			// set the executing device to null, which indicates that there is
			// no active context; this is used by machine.time() to return the
			// context-appropriate value
			m_executing_device = nullptr;

			// our final target becomes our new base time
			basetime = target;
		}

		// set the new basetime globally so that timers see it
		m_basetime.set_relative(basetime);

		// now that we've reached the expiration time of the first timer in the
		// queue, execute pending ones
		execute_timers(m_basetime.absolute());

	} while (basetime < endslice && !m_hard_stopping);

	// at this point we'll say we're out of the timeslice
	m_in_timeslice = false;

	// if we are hard stopping, undo some of the damage on the way out
	if (m_hard_stopping)
		hard_stop_complete();
}


//-------------------------------------------------
//  timeslice_partial -- execute a partial
//  timeslice after a mid-slice restore
//-------------------------------------------------

void device_scheduler::timeslice_partial()
{
	// should only get here if we have a target device to advance to
	scheduler_assert(m_save_executing != -1);

	// officially within a timeslice now; clear any hard stopping state
	m_in_timeslice = true;
	m_hard_stopping = false;

	INCREMENT_SCHEDULER_STAT(m_timeslice);

	// don't mess with basetime; it was just restored and should be in good shape

	// run at least the given number of subseconds
	subseconds basetime = m_basetime.relative();

	// no need to compute endslice, since the timing comes from the save state
	INCREMENT_SCHEDULER_STAT(m_timeslice_inner1);

	LOG("------------------\n");

	// do not check quantum expiration here
	INCREMENT_SCHEDULER_STAT(m_timeslice_inner2);

	// target time comes from the save state
	subseconds target = m_save_target;

	// don't check timers; this was already done before saving
	LOG("timeslice: target = %sas\n", attotime(target).as_string(18));

	// do we have pending suspension changes? (just refresh; don't advance)
	apply_suspend_changes(false);

	// determine the starting device to execute (normally the first one unless
	// restoring from a mid-timeslice save state)
	device_execute_interface *exec = m_execute_list;

	// find the indexed device
	while (m_save_executing-- != 0 && exec != nullptr)
		exec = exec->m_nextexec;
	if (exec == nullptr)
		throw emu_fatalerror("Attempted mid-timeslice restore but ran out of devices");
	scheduler_assert(m_save_executing == -1);

	// if the debugger is enabled, halt on the first instruction
	if (exec->debugger_enabled())
		machine().debugger().cpu().set_execution_stopped();

	// loop over all executing devices
	bool first = true;
	for ( ; exec != nullptr; exec = exec->m_nextexec)
	{
		// compute how many subseconds to execute this device
		subseconds delta = target - exec->m_localtime.relative() - subseconds::unit();

		// if we're the first device, backwards compute delta from the icount remaining
		if (first)
		{
			delta = exec->cycles_to_attotime(m_save_icount - 1).as_subseconds() - subseconds::unit();
			first = false;
		}

		// if we're already ahead, do nothing; in theory we should do this 0 as
		// well, but it's a rare case and the compiler tends to be able to
		// optimize a strict < 0 check better than <= 0
		if (delta < subseconds::zero())
			continue;

		INCREMENT_SCHEDULER_STAT(m_timeslice_inner3);

#if VERBOSE
		u64 start_cycles = exec->total_cycles();
		LOG("  %12.12s: t=%018I64das %018I64das = %dc; ", exec->device().tag(), exec->m_localtime.relative().raw(), delta.raw(), delta / exec->m_subseconds_per_cycle + 1);
#endif

		// store a pointer to the executing device so that we know the
		// relevant active context
		m_executing_device = exec;

		// execute for the given number of subseconds
		subseconds localtime = exec->run_for(delta);

#if VERBOSE
		m_executing_device = nullptr;
		u64 end_cycles = exec->total_cycles();
		LOG(" ran %dc, %1I64dc total", s32(end_cycles - start_cycles), end_cycles);
#endif

		// if the new local device time is less than our target, move the
		// target up, but not before the base
		if (UNEXPECTED(localtime < target))
		{
			m_save_target = target = std::max(localtime, basetime);
			LOG(" (new target)");
		}
		LOG("\n");
	}

	// set the executing device to null, which indicates that there is
	// no active context; this is used by machine.time() to return the
	// context-appropriate value
	m_executing_device = nullptr;

	// our final target becomes our new base time
	m_basetime.set_relative(target);

	// no longer in a timeslice
	m_in_timeslice = false;
}


//-------------------------------------------------
//  execute_timers - execute timers that are due
//-------------------------------------------------

inline void device_scheduler::execute_timers(attotime const &basetime)
{
	LOG("execute_timers: new=%s head->expire=%s\n", basetime.as_string(PRECISION), m_first_timer_expire.absolute().as_string(PRECISION));

	INCREMENT_SCHEDULER_STAT(m_execute_timers);

	// now process any timers that are overdue; due to our never-expiring dummy
	// instance, we don't need to check for nullptr on the head
	while (m_active_timers_head->m_expire <= basetime && !m_hard_stopping)
	{
		INCREMENT_SCHEDULER_STAT(m_execute_timers_average);

		// pull the timer off the head of the queue
		timer_instance &timer = *m_active_timers_head;
		m_active_timers_head = timer.m_next;
		m_active_timers_head->m_prev = nullptr;
		timer.m_active = false;

		// set the global state of which callback we're in
		m_callback_timer = &timer;
		m_callback_timer_expire_time = timer.m_expire;

		// call the callback
		g_profiler.start(PROFILER_TIMER_CALLBACK);
		{
			if (timer.is_device_timer())
				LOG("execute_timers: timer device %s timer %d\n", timer.m_callback->device()->tag(), timer.id());
			else
				LOG("execute_timers: timer callback %s\n", timer.m_callback->name());

			(*timer.m_callback)(timer);
		}
		g_profiler.stop();

		// reclaim the timer now that we're done with it
		instance_reclaim(timer);
	}

	// update the expiration time of the first timer
	update_first_timer_expire();

	// clear the callback timer global
	m_callback_timer = nullptr;
}


//-------------------------------------------------
//  update_basetime - update all the
//  basetime_relative times now that the basetime
//  has ticked over another second
//-------------------------------------------------

void device_scheduler::update_basetime()
{
	INCREMENT_SCHEDULER_STAT(m_update_basetime);

	// rebase the basetime itself
	attotime basetime(m_basetime.absolute().seconds());
	m_basetime.set_base(basetime);

	// update execute devices
	for (device_execute_interface &exec : execute_interface_enumerator(machine().root_device()))
		exec.m_localtime.set_base(basetime);

	// update quantum expirations
	for (int index = 0; index < m_quantum_count; index++)
		m_quantum_slot[index].m_expire.set_base(basetime);

	// move timers from future list into current list
	m_first_timer_expire.set_base(basetime);
}


//-------------------------------------------------
//  hard_stop_complete - complete a hard stop
//  request by putting things back in order and
//  potentially initiating a state load
//-------------------------------------------------

void device_scheduler::hard_stop_complete()
{
	// reset the state
	scheduler_assert(m_hard_stopping);
	m_hard_stopping = false;

	// hard stopping can trash the execute list; rebuild it
	rebuild_execute_list();

	// also compute the true first time expiraton
	update_first_timer_expire();

	// perform the load if that's what we're to do
	if (m_load_after_stop)
		machine().immediate_load();
}


//-------------------------------------------------
//  rebuild_execute_list - create a fast list of
//  executing devices
//-------------------------------------------------

void device_scheduler::rebuild_execute_list()
{
	//
	device_execute_interface **tailptr = &m_execute_list;
	*tailptr = nullptr;
	for (device_execute_interface &exec : execute_interface_enumerator(machine().root_device()))
	{
		exec.m_nextexec = nullptr;
		*tailptr = &exec;
		tailptr = &exec.m_nextexec;
	}
}


//-------------------------------------------------
//  compute_perfect_interleave - compute the
//  "perfect" interleave interval
//-------------------------------------------------

void device_scheduler::compute_perfect_interleave()
{
	INCREMENT_SCHEDULER_STAT(m_compute_perfect_interleave);

	// skip if nothing or only one item on our list
	if (m_execute_list == nullptr || m_execute_list->m_nextexec == nullptr)
	{
		m_quantum_minimum = MAX_QUANTUM;
		return;
	}

	// start with a huge time factor and find the 2nd smallest cycle time
	subseconds smallest = m_execute_list->minimum_quantum();
	subseconds perfect = MAX_QUANTUM;
	for (device_execute_interface *exec = m_execute_list->m_nextexec; exec != nullptr; exec = exec->m_nextexec)
	{
		// find the 2nd smallest cycle interval
		subseconds curquantum = exec->minimum_quantum();
		if (curquantum < smallest)
		{
			perfect = smallest;
			smallest = curquantum;
		}
		else if (curquantum < perfect)
			perfect = curquantum;
	}

	// if this is a new minimum quantum, apply it
	if (m_quantum_minimum != perfect)
	{
		// adjust all the actuals; this doesn't affect the current
		m_quantum_minimum = perfect;
		for (int index = 0; index < m_quantum_count; index++)
			m_quantum_slot[index].m_actual = std::max(m_quantum_slot[index].m_requested, m_quantum_minimum);
	}
}


//-------------------------------------------------
//  apply_suspend_changes - applies suspend/resume
//  changes to all device_execute_interfaces
//-------------------------------------------------

inline void device_scheduler::apply_suspend_changes(bool advance)
{
	INCREMENT_SCHEDULER_STAT(m_apply_suspend_changes);

	// update the suspend state on all executing devices
	m_suspend_changes_pending = false;
	for (device_execute_interface *exec = m_execute_list; exec != nullptr; exec = exec->m_nextexec)
		if (exec->update_suspend(advance))
			m_suspend_changes_pending = true;
}


//-------------------------------------------------
//  add_scheduling_quantum - add a scheduling
//  quantum; the smallest active one is the one
//  that is in use
//-------------------------------------------------

void device_scheduler::add_scheduling_quantum(subseconds quantum, attotime const &duration)
{
	// ignore quanta larger than the maximum
	if (quantum > MAX_QUANTUM)
		return;
	if (m_quantum_count == MAX_ACTIVE_QUANTA)
	{
		osd_printf_error("Ran out of scheduling quanta!");
		return;
	}

	INCREMENT_SCHEDULER_STAT(m_add_scheduling_quantum);

	// compute the expiration time
	attotime curtime = time();
	attotime expire = curtime + duration;

	// figure out where to insert ourselves, expiring any quanta that are out-of-date
	int index;
	for (index = 0; index < m_quantum_count; index++)
	{
		quantum_slot &quant = m_quantum_slot[index];

		// if this quantum is expired, pull it from the list and add it to the free list
		if (curtime >= quant.m_expire.absolute())
		{
			m_quantum_count--;
			memmove(&m_quantum_slot[index], &m_quantum_slot[index + 1], (m_quantum_count - index) * sizeof(m_quantum_slot[0]));
			index--;
		}

		// if the new quantum is equal, just merge entries
		else if (quantum == quant.m_requested)
		{
			if (expire > quant.m_expire.absolute())
				quant.m_expire.set(expire);
			return;
		}

		// if the new quantum is smaller, we want to be inserted before this entry
		else if (quantum < quant.m_requested)
			break;
	}

	// make room for the new item
	memmove(&m_quantum_slot[index + 1], &m_quantum_slot[index], (m_quantum_count - index) * sizeof(m_quantum_slot[0]));
	m_quantum_count++;

	// fill it out
	quantum_slot &newquant = m_quantum_slot[index];
	newquant.m_requested = quantum;
	newquant.m_actual = std::max(quantum, m_quantum_minimum);
	newquant.m_expire.set(expire);
}


//-------------------------------------------------
//  instance_alloc - allocate memory for a new
//  timer instance, either by reclaiming a
//  freed one, or allocating memory for a new one
//-------------------------------------------------

timer_instance &device_scheduler::instance_alloc()
{
	INCREMENT_SCHEDULER_STAT(m_instance_alloc);

	// attempt to rescue one off the free list
	timer_instance *instance = m_free_timers;
	if (instance != nullptr)
	{
		m_free_timers = instance->m_next;
		return *instance;
	}

	INCREMENT_SCHEDULER_STAT(m_instance_alloc_full);

	// if none, allocate a new one
	m_allocated_instances.push_back(std::make_unique<timer_instance>());
	return *m_allocated_instances.back().get();
}


//-------------------------------------------------
//  instance_reclaim - reclaim memory for a
//  timer instance by adding it to the free list
//-------------------------------------------------

inline void device_scheduler::instance_reclaim(timer_instance &timer)
{
	// don't reclaim persistent instances because they are part of the
	// persistent_timer object
	if (timer.m_callback->persistent() != nullptr)
		return;

	// reclaimed instances go back on the free list
	timer.m_next = m_free_timers;
	m_free_timers = &timer;
}


//-------------------------------------------------
//  instance_insert - insert a timer instance at
//  the the appropriate spot in the active
//  timer queue
//-------------------------------------------------

inline timer_instance &device_scheduler::instance_insert(timer_instance &timer)
{
	// special case insert at start; we always have at least a dummy never-
	// expiring timer in the list, so no need to check for nullptr
	if (timer.m_expire < m_active_timers_head->m_expire)
	{
		INCREMENT_SCHEDULER_STAT(m_instance_insert_head);

		// no previous, next is the head
		timer.m_prev = nullptr;
		timer.m_next = m_active_timers_head;

		// link the old head as the previous and make us head
		m_active_timers_head = m_active_timers_head->m_prev = &timer;

		// since the head changed, the time of the first expirations changed
		update_first_timer_expire();
		abort_timeslice();
		return timer;
	}

	// special case insert at end; since we're not the head, we can be sure
	// that there's at least one timer before the permanent tail
	if (timer.m_expire >= m_active_timers_tail.m_prev->m_expire)
	{
		INCREMENT_SCHEDULER_STAT(m_instance_insert_tail);

		// hook us up in front of the tail
		timer.m_prev = m_active_timers_tail.m_prev;
		timer.m_next = &m_active_timers_tail;
		m_active_timers_tail.m_prev = m_active_timers_tail.m_prev->m_next = &timer;

		// no need to recompute if changing a later timer
		return timer;
	}

	INCREMENT_SCHEDULER_STAT(m_instance_insert_middle);

	// scan to find out where we go
	for (timer_instance *scan = m_active_timers_head->m_next; scan != nullptr; scan = scan->m_next)
	{
		INCREMENT_SCHEDULER_STAT(m_instance_insert_average);
		if (timer.m_expire < scan->m_expire)
		{
			timer.m_prev = scan->m_prev;
			timer.m_next = scan;
			scan->m_prev = scan->m_prev->m_next = &timer;

			// no need to recompute if changing a later timer
			return timer;
		}
	}

	// should never get here
	return timer;
}


//-------------------------------------------------
//  instance_remove - remove a timer from the
//  active timer queue
//-------------------------------------------------

inline timer_instance &device_scheduler::instance_remove(timer_instance &timer)
{
	INCREMENT_SCHEDULER_STAT(m_instance_remove);

	// link the previous to us; if no previous, we're the head
	if (timer.m_prev != nullptr)
		timer.m_prev->m_next = timer.m_next;
	else
	{
		m_active_timers_head = timer.m_next;
		update_first_timer_expire();
	}

	// link the next to us; we can't be the tail, so presume next is non-null
	timer.m_next->m_prev = timer.m_prev;

	// return the timer back for chaining
	return timer;
}


//-------------------------------------------------
//  empty_timer - empty callback stub when
//  timers provide nothing
//-------------------------------------------------

void device_scheduler::empty_timer(timer_instance const &timer)
{
	INCREMENT_SCHEDULER_STAT(m_empty_timer_calls);
}


//-------------------------------------------------
//  timed_trigger - generate a trigger after a
//  given amount of time
//-------------------------------------------------

void device_scheduler::timed_trigger(timer_instance const &timer)
{
	INCREMENT_SCHEDULER_STAT(m_timed_trigger_calls);
	trigger(timer.param());
}
