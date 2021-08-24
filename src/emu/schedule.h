// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    schedule.h

    Core device execution and scheduling engine.

***************************************************************************/

#pragma once

#ifndef __EMU_H__
#error Dont include this file directly; include emu.h instead.
#endif

#ifndef MAME_EMU_SCHEDULE_H
#define MAME_EMU_SCHEDULE_H


//**************************************************************************
//  DEBUGGING
//**************************************************************************

// turn this on to enable aggressive assertions and other checks
#ifdef MAME_DEBUG
#define SCHEDULER_DEBUG (1)
#else
#define SCHEDULER_DEBUG (1)
#endif

// if SCHEDULER_DEBUG is on, make assertions fire regardless of MAME_DEBUG
#if (SCHEDULER_DEBUG)
#define scheduler_assert(x) do { if (!(x)) { osd_printf_error("scheduler_assert: " #x "\n"); osd_break_into_debugger("scheduler_assert: " #x "\n"); } } while (0)
#else
#define scheduler_assert assert
#endif

#define COLLECT_SCHEDULER_STATS (1)
#if (COLLECT_SCHEDULER_STATS)
#define INCREMENT_SCHEDULER_STAT(x) do { x += 1; } while (0)
#define SET_SCHEDULER_STAT(x, y) do { x = y; } while (0)
#else
#define INCREMENT_SCHEDULER_STAT(x)
#define SET_SCHEDULER_STAT(x, y)
#endif


//**************************************************************************
//  MACROS
//**************************************************************************

#define TIMER_CALLBACK_MEMBER(name)     void name(void *ptr, s32 param)


//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

// forward definitions
class persistent_timer;

// timer IDs for devices
using device_timer_id = u32;

// timer callbacks look like this natively
using timer_expired_delegate_native = named_delegate<void (timer_instance const &)>;

// alternate form #1 takes no parameters
using timer_expired_delegate_form1 = delegate<void ()>;

// alternate form #2 takes a single integer parameter of any type
template<typename IntType>
using timer_expired_delegate_form2 = delegate<void (IntType)>;

// alternate form #3 takes a pointer value and an integer parameter; this is the classic TIMER_CALLBACK
template<typename IntType>
using timer_expired_delegate_form3 = delegate<void (void *, IntType)>;

// alternate form #4 takes two integer parameters of any type; maps to some write handlers
template<typename IntType, typename IntType2>
using timer_expired_delegate_form4 = delegate<void (IntType, IntType2)>;

// alternate form #5 takes three integer parameters of any type; maps to some write handlers
template<typename IntType, typename IntType2, typename IntType3>
using timer_expired_delegate_form5 = delegate<void (IntType, IntType2, IntType3)>;


// ======================> timer_expired_delegate

// a timer_expired_delegate represents a bound timer expired callback; it can wrap
// all of the above alternate forms via built-in trampolines
class timer_expired_delegate : public timer_expired_delegate_native
{
	// this is just a substitute for an arbitrary delegate; it presumes that
	// all delegates are equivalent from a size/copy/move perspective
	using generic_delegate = delegate<void ()>;

public:
	// import direct constructors for native callbacks
	using timer_expired_delegate_native::timer_expired_delegate_native;

	// copy constructor
	timer_expired_delegate(timer_expired_delegate const &src) :
		timer_expired_delegate_native(src),
		m_sub_delegate(src.m_sub_delegate)
	{
		// if the delegate is bound to the source object, rebind it to the copy
		if (src.has_sub_delegate())
			bind(reinterpret_cast<delegate_generic_class *>(this));
		SET_SCHEDULER_STAT(m_form, src.m_form);
	}

	// copy assignment
	timer_expired_delegate &operator=(timer_expired_delegate const &src)
	{
		// copy the native and sub delegates
		*static_cast<timer_expired_delegate_native *>(this) = src;
		m_sub_delegate = src.m_sub_delegate;
		SET_SCHEDULER_STAT(m_form, src.m_form);

		// if the delegate is bound to the source object, rebind it to the copy
		if (src.has_sub_delegate())
			bind(reinterpret_cast<delegate_generic_class *>(this));
		return *this;
	}

	// equality
	bool operator==(const timer_expired_delegate &rhs) const
	{
		if (has_sub_delegate())
			return rhs.has_sub_delegate() ? (m_sub_delegate == rhs.m_sub_delegate) : false;
		else
			return rhs.has_sub_delegate() ? false : timer_expired_delegate_native::operator==(rhs);
	}
	bool operator!=(const timer_expired_delegate &rhs) const
	{
		if (has_sub_delegate())
			return rhs.has_sub_delegate() ? (m_sub_delegate != rhs.m_sub_delegate) : false;
		else
			return rhs.has_sub_delegate() ? false : timer_expired_delegate_native::operator!=(rhs);
	}

	// form 1 constructor: void timer_callback()
	template<typename FuncDeviceType, typename DeviceType>
	timer_expired_delegate(void (FuncDeviceType::*cb)(), char const *name, DeviceType *bindto) :
		timer_expired_delegate_native(&timer_expired_delegate::form1_callback, name, this)
	{
		static_assert(sizeof(timer_expired_delegate_form1) == sizeof(m_sub_delegate));
		reinterpret_cast<timer_expired_delegate_form1 &>(m_sub_delegate) = timer_expired_delegate_form1(cb, bindto);
		SET_SCHEDULER_STAT(m_form, 1);
	}

	// form 2 constructor: void timer_callback(int param)
	template<typename FuncDeviceType, typename DeviceType, typename IntType, std::enable_if_t<std::is_integral<IntType>::value, bool> = true>
	timer_expired_delegate(void (FuncDeviceType::*cb)(IntType), char const *name, DeviceType *bindto) :
		timer_expired_delegate_native(&timer_expired_delegate::form2_callback<IntType>, name, this)
	{
		static_assert(sizeof(timer_expired_delegate_form2<IntType>) == sizeof(m_sub_delegate));
		reinterpret_cast<timer_expired_delegate_form2<IntType> &>(m_sub_delegate) = timer_expired_delegate_form2<IntType>(cb, bindto);
		SET_SCHEDULER_STAT(m_form, 2);
	}

	// form 3 constructor: void timer_callback(void *ptr, int param)
	template<typename FuncDeviceType, typename DeviceType, typename IntType, std::enable_if_t<std::is_integral<IntType>::value, bool> = true>
	timer_expired_delegate(void (FuncDeviceType::*cb)(void *ptr, IntType), char const *name, DeviceType *bindto) :
		timer_expired_delegate_native(&timer_expired_delegate::form3_callback<IntType>, name, this)
	{
		static_assert(sizeof(timer_expired_delegate_form3<IntType>) == sizeof(m_sub_delegate));
		reinterpret_cast<timer_expired_delegate_form3<IntType> &>(m_sub_delegate) = timer_expired_delegate_form3<IntType>(cb, bindto);
		SET_SCHEDULER_STAT(m_form, 3);
	}

	// form 4 constructor: void timer_callback(int param, int param2)
	template<typename FuncDeviceType, typename DeviceType, typename IntType, typename IntType2, std::enable_if_t<std::is_integral<IntType>::value && std::is_integral<IntType2>::value, bool> = true>
	timer_expired_delegate(void (FuncDeviceType::*cb)(IntType, IntType2), char const *name, DeviceType *bindto) :
		timer_expired_delegate_native(&timer_expired_delegate::form4_callback<IntType, IntType2>, name, this)
	{
		static_assert(sizeof(timer_expired_delegate_form4<IntType, IntType2>) == sizeof(m_sub_delegate));
		reinterpret_cast<timer_expired_delegate_form4<IntType, IntType2> &>(m_sub_delegate) = timer_expired_delegate_form4<IntType, IntType2>(cb, bindto);
		SET_SCHEDULER_STAT(m_form, 4);
	}

	// form 5 constructor: void timer_callback(int param, int param2, int param3)
	template<typename FuncDeviceType, typename DeviceType, typename IntType, typename IntType2, typename IntType3, std::enable_if_t<std::is_integral<IntType>::value && std::is_integral<IntType2>::value && std::is_integral<IntType3>::value, bool> = true>
	timer_expired_delegate(void (FuncDeviceType::*cb)(IntType, IntType2, IntType3), char const *name, DeviceType *bindto) :
		timer_expired_delegate_native(timer_expired_delegate::form5_callback<IntType, IntType2, IntType3>, name, this)
	{
		static_assert(sizeof(timer_expired_delegate_form5<IntType, IntType2, IntType3>) == sizeof(m_sub_delegate));
		reinterpret_cast<timer_expired_delegate_form5<IntType, IntType2, IntType3> &>(m_sub_delegate) = timer_expired_delegate_form5<IntType, IntType2, IntType3>(cb, bindto);
		SET_SCHEDULER_STAT(m_form, 5);
	}

	// return the name
	char const *name() const { return timer_expired_delegate_native::name(); }

#if (COLLECT_SCHEDULER_STATS)
	int m_form = 0;
#endif

private:
	// helper: true if this uses a subdelegate
	bool has_sub_delegate() const
	{
		return (object() == const_cast<delegate_generic_class *>(reinterpret_cast<delegate_generic_class const *>(this)));
	}

	// callbacks for various forms
	void form1_callback(timer_instance const &timer);
	template<typename IntType> void form2_callback(timer_instance const &timer);
	template<typename IntType> void form3_callback(timer_instance const &timer);
	template<typename IntType, typename IntType2> void form4_callback(timer_instance const &timer);
	template<typename IntType, typename IntType2, typename IntType3> void form5_callback(timer_instance const &timer);

	// secondary delegate, which may be of a number of forms
	generic_delegate m_sub_delegate;
};


// ======================> timer_callback

// a timer_callback represents a registered callback, along with a user-supplied
// pointer and other useful information; timer_callbacks are used internally by
// both the persistent_timer and transitent_timer_factory classes
class timer_callback
{
	friend class device_scheduler;
	friend class persistent_timer;

public:
	// construction/destruction
	timer_callback(persistent_timer *persistent = nullptr);
	~timer_callback();

	// copy constructor
	timer_callback(timer_callback const &src);

	// copy assignment
	timer_callback &operator=(timer_callback const &src);

	// calling operator
	void operator()(timer_instance const &timer) { INCREMENT_SCHEDULER_STAT(m_calls); m_delegate(timer); }

	// registration of a delegate directly
	timer_callback &init(device_scheduler &scheduler, timer_expired_delegate const &delegate, char const *unique = nullptr, char const *unique2 = nullptr)
	{
		return init_base(&scheduler, delegate, unique, unique2);
	}

	// registration of an arbitrary member function bound to an arbitrary object; requires the
	// device_scheduler as the first parameter since we don't know how to get one
	template<typename ObjectType, typename FuncType>
	std::enable_if_t<std::is_member_function_pointer<FuncType>::value, timer_callback &> init(device_scheduler &scheduler, ObjectType &object, FuncType callback, char const *string, char const *unique = nullptr)
	{
		return init_base(&scheduler, timer_expired_delegate(callback, string, &object), unique);
	}

	// registration of a device member function bound to that device
	template<typename DeviceType, typename FuncType>
	std::enable_if_t<std::is_base_of<device_t, DeviceType>::value && std::is_member_function_pointer<FuncType>::value, timer_callback &> init(DeviceType &device, FuncType callback, char const *string, char const *unique = nullptr)
	{
		return init_device(device, timer_expired_delegate(callback, string, &device), unique);
	}

	// registration of a device interface member function bound to the interface
	// this is only enabled if the call is NOT a device_t (to prevent ambiguity)
	template<typename IntfType, typename FuncType>
	std::enable_if_t<std::is_base_of<device_interface, IntfType>::value && !std::is_base_of<device_t, IntfType>::value, timer_callback &> init(IntfType &intf, FuncType callback, char const *string, char const *unique = nullptr)
	{
		return init_device(intf.device(), timer_expired_delegate(callback, string, &intf), unique);
	}

	// getters
	bool is_initialized() const { return (m_scheduler != nullptr); }
	device_scheduler &scheduler() const { assert(m_scheduler != nullptr); return *m_scheduler; }
	char const *name() const { return m_delegate.name(); }
	void *ptr() const { return m_ptr; }
	device_t *device() const { return m_device; }
	persistent_timer *persistent() const { return m_persistent; }
	u32 unique_hash() const { return m_unique_hash; }
	u32 save_index() const { return m_save_index; }
	char const *unique_id() const { return m_unique_id.c_str(); }

	// setters
	timer_callback &set_ptr(void *ptr);
	timer_callback &set_device(device_t &device);

private:
	// registration helpers
	timer_callback &init_base(device_scheduler *scheduler, timer_expired_delegate const &delegate, char const *unique = nullptr, char const *unique2 = nullptr);
	timer_callback &init_device(device_t &device, timer_expired_delegate const &delegate, char const *unique);
	timer_callback &init_clone(timer_callback const &src, timer_expired_delegate const &delegate);

	// internal state
	timer_expired_delegate m_delegate;  // the full delegate
	void *m_ptr;                        // user-supplied pointer
	device_scheduler *m_scheduler;      // pointer to the scheduler
	timer_callback *m_next_registered;  // link to the next registered item
	persistent_timer *m_persistent;     // pointer to our owning persistent timer, or nullptr
	device_t *m_device;                 // pointer to device, for debugging/logging
	u32 m_unique_hash;                  // hash of the unique ID
	u32 m_save_index;                   // index for saving
#if (COLLECT_SCHEDULER_STATS)
	u64 m_calls = 0;                    // number of calls made
#endif
	std::string m_unique_id;            // a unique ID string
};


// ======================> timer_instance_save

// timer_instance_save is an internal structure that holds a single saved
// timer instance, for active transient timers
struct timer_instance_save
{
	attotime start;                     // saved/restore by timer_instance
	attotime expire;                    // saved/restore by timer_instance
	u64 param[3];                       // saved/restore by timer_instance
	u32 hash;                           // saved/restore by timer_instance/persistent_timer
	u32 save_index;                     // saved/restore by persistent_timer

	void register_save(save_manager &save, int index)
	{
		save.save_item(nullptr, "timer_instance", "transient", index, NAME(start));
		save.save_item(nullptr, "timer_instance", "transient", index, NAME(expire));
		save.save_item(nullptr, "timer_instance", "transient", index, NAME(param));
		save.save_item(nullptr, "timer_instance", "transient", index, NAME(hash));
		save.save_item(nullptr, "timer_instance", "transient", index, NAME(save_index));
	}
};


// ======================> timer_instance

// a timer_instance represents an intantiated instance of a timer; for persistent
// timers, there is one timer_instance embedded as part of the presistent_timer
// object; for transient timers, timer_instances are allocated on the fly whenever
// the transient_timer_factory is requested to issue a callback
class timer_instance
{
	friend class device_scheduler;
	friend class persistent_timer;
	friend class transient_timer_factory;

	DISABLE_COPYING(timer_instance);

public:
	// construction/destruction
	timer_instance();
	~timer_instance();

	// allocation and re-use
	timer_instance &init_transient(timer_callback &callback, attotime const &duration, bool absolute);
	timer_instance &init_persistent(timer_callback &callback);

	// getters
	device_scheduler &scheduler() const noexcept { return m_callback->scheduler(); }
	timer_instance *prev() const { return m_prev; }
	timer_instance *next() const { return m_next; }
	u64 param(int index = 0) const { scheduler_assert(m_callback == nullptr || index < (is_device_timer() ? 2 : 3)); return m_param[index]; }
	void *ptr() const { return m_callback->ptr(); }
	bool active() const { return m_active; }

	// device timer-specific getters
	device_timer_id id() const { scheduler_assert(is_device_timer()); return device_timer_id(m_param[2]); }
	bool is_device_timer() const { return (m_callback->device() != nullptr); }

	// timing queries
	attotime elapsed() const noexcept;
	attotime remaining() const noexcept;
	attotime const &start() const { return m_start; }
	attotime const &expire() const { return m_expire; }

	// save state for persistent timers that own us
	void register_save(save_manager &save)
	{
		save.save_item(nullptr, "timer_instance", m_callback->unique_id(), m_callback->save_index(), NAME(m_start));
		save.save_item(nullptr, "timer_instance", m_callback->unique_id(), m_callback->save_index(), NAME(m_expire));
		save.save_item(nullptr, "timer_instance", m_callback->unique_id(), m_callback->save_index(), NAME(m_param));
	}

private:
	// internal setters
	timer_instance &set_param(int index, u64 param) { m_param[index] = param; return *this; }
	timer_instance &set_param(u64 param) { return set_param(0, param); }
	timer_instance &set_params(u64 param0, u64 param1) { return set_param(0, param0).set_param(1, param1); }
	timer_instance &set_params(u64 param0, u64 param1, u64 param2) { return set_param(0, param0).set_param(1, param1).set_param(2, param2); }

	// internal helpers
	timer_instance &save(timer_instance_save &dst);
	timer_instance &restore(timer_instance_save const &src, timer_callback &callback);
	timer_instance &insert(attotime const &start, attotime const &expire);
	timer_instance &remove();
	void dump() const;

	// internal state
	timer_instance *    m_next;         // next timer in order in the list
	timer_instance *    m_prev;         // previous timer in order in the list
	attotime            m_start;        // time when the timer was started
	attotime            m_expire;       // time when the timer will expire
	timer_callback *    m_callback;     // pointer to the external callback
	u64                 m_param[3];     // integer parameters
	bool                m_active;       // true if currently in the active list
};


// ======================> transient_timer_factory

// a transient_timer_factory contains a timer_callback and can dynamically
// create multiple timer_instances that call the callback after a certain
// elapsed time; these timers are fire-and-forget, and it is not possible to
// modify or cancel them once issued
class transient_timer_factory
{
	friend class device_t;

	DISABLE_COPYING(transient_timer_factory);

public:
	// constructor
	transient_timer_factory();

	// initialization
	template<typename... T>
	transient_timer_factory &init(T &&... args)
	{
		m_callback.init(std::forward<T>(args)...);
		return *this;
	}

	// getters
	timer_callback const &callback() const { return m_callback; }

	// create a new timer_instance that will fire after the given duration
	void call_after(attotime const &duration, u64 param = 0, u64 param2 = 0, u64 param3 = 0);
	void call_at(attotime const &abstime, u64 param = 0, u64 param2 = 0, u64 param3 = 0);

	// create a new timer_instance that will fire as soon as possible
	void synchronize(u64 param = 0, u64 param2 = 0, u64 param3 = 0)
	{
		call_after(attotime::zero, param, param2, param3);
	}

private:
	// internal state
	timer_callback m_callback;          // the embedded callback
};


// ======================> persistent_timer

// a persistent_timer contains a time_callback and a timer_instance, which
// can be manipulated
class persistent_timer
{
	friend class device_scheduler;

	DISABLE_COPYING(persistent_timer);

public:
	// construction/destruction
	persistent_timer();
	virtual ~persistent_timer();

	// initialization
	template<typename... T>
	persistent_timer &init(T &&... args)
	{
		m_callback.init(std::forward<T>(args)...);
		return init_common();
	}

	// getters
	timer_instance const &instance() const { return m_instance; }
	timer_callback const &callback() const { return m_callback; }
	u64 param(int index = 0) const { return m_instance.param(index); }
	void *ptr() const { return m_callback.ptr(); }
	bool enabled() const { return m_enabled && m_instance.active(); }
	bool periodic() const { return !m_period.is_never(); }
	attotime elapsed() const noexcept { return m_instance.elapsed(); }
	attotime remaining() const noexcept { return m_instance.remaining(); }
	attotime const &start() const { return m_instance.start(); }
	attotime const &expire() const { return m_instance.expire(); }
	attotime const &period() const { return m_period; }

	// setters
	persistent_timer &set_param(int index, u64 param) { m_instance.set_param(index, param); return *this; }
	persistent_timer &set_param(u64 param) { return set_param(0, param); }
	persistent_timer &set_params(u64 param0, u64 param1) { return set_param(0, param0).set_param(1, param1); }
	persistent_timer &set_params(u64 param0, u64 param1, u64 param2) { return set_param(0, param0).set_param(1, param1).set_param(2, param2); }
	persistent_timer &set_ptr(void *ptr) { m_callback.set_ptr(ptr); m_periodic_callback.set_ptr(ptr); return *this; }

	// control
	bool enable(bool enable = true);
	bool disable() { return enable(false); }
	persistent_timer &reset(attotime const &duration = attotime::never) { return adjust(duration, m_instance.param(), m_period); }
	persistent_timer &adjust(attotime const &start_delay, s32 param = 0, attotime const &periodicity = attotime::never) { return adjust_internal(start_delay, param, periodicity, false); }
	persistent_timer &adjust_absolute(attotime const &start_time, s32 param = 0, attotime const &periodicity = attotime::never) { return adjust_internal(start_time, param, periodicity, true); }

	// save state
	void register_save(save_manager &save, int index)
	{
		save.save_item(nullptr, "persistent_timer", m_callback.unique_id(), m_callback.save_index(), NAME(m_period));
		save.save_item(nullptr, "persistent_timer", m_callback.unique_id(), m_callback.save_index(), NAME(m_enabled));
		m_instance.register_save(save);
	}

protected:
	// internal helpers
	void periodic_callback(timer_instance const &timer);
	persistent_timer &init_common();
	persistent_timer &restore(timer_instance_save const &src, timer_callback &callback);
	persistent_timer &adjust_internal(attotime const &delay, s32 param, attotime const &periodicity, bool absolute);

	// internal state
	attotime m_period;                  // the timer period, or attotime::never if not periodic
	bool m_enabled;                     // true if enabled, false if disabled
	bool m_modified;                    // true if modified
	timer_instance m_instance;          // the embedded timer instance
	timer_callback m_callback;          // the embedded timer callback
	timer_callback m_periodic_callback; // a wrapper callback for periodic timers
};

// eventually replace emu_timer with persistent_timer
using emu_timer = persistent_timer;


// ======================> device_scheduler

class device_scheduler
{
	friend class device_execute_interface;
	friend class transient_timer_factory;
	friend class timer_instance;

	// due to save state limitations these have to be fixed
	static constexpr int TIMER_SAVE_SLOTS = 256;
	static constexpr int MAX_ACTIVE_QUANTA = 16;

	// inner private class for maintaining base-time relative values for
	// faster comparisons vs a full attotime
	class basetime_relative
	{
	public:
		// construction/destruction
		basetime_relative() : m_absolute_dirty(false) { }

		// set an absolute time
		void set(attotime const &src) { m_absolute = src; m_absolute_dirty = false; update_relative(); }

		// set a relative time
		void set_relative(subseconds rel) { m_relative = rel; m_absolute_dirty = true; }

		// add a number of subseconds to the relative time
		void add(subseconds src) { m_relative += src; m_absolute_dirty = true; }

		// return the relative time
		subseconds relative() const { return m_relative; }

		// return the absolute time, updating if dirty
		attotime const &absolute() { if (m_absolute_dirty) update_absolute(); return m_absolute; }

		// return the absolute time, with no updating
		attotime const &absolute_no_update() const { return m_absolute; }

		// return the base time
		attotime const &base() const { return m_base; }

		// set the base for the relative time
		void set_base(attotime const &base) { if (m_absolute_dirty) update_absolute(); m_base = base; update_relative(); }

	private:
		// internal helpers
		void update_relative() { m_relative = (m_absolute - m_base).as_subseconds(); }
		void update_absolute() { m_absolute = m_base + m_relative; m_absolute_dirty = false; }

	public:
		// internal state, public for saving
		subseconds m_relative;
		attotime m_absolute;
		attotime m_base;
		bool m_absolute_dirty;
	};

public:
	// construction/destruction
	device_scheduler(running_machine &machine);
	~device_scheduler();

	// setup
	void finalize();

	// getters
	running_machine &machine() const noexcept { return m_machine; }
	attotime time() const noexcept;
	device_execute_interface *currently_executing() const noexcept { return m_executing_device; }
	bool in_timeslice() const { return m_in_timeslice; }
	bool hard_stopping() const { return m_hard_stopping; }

	// execution
	void timeslice(subseconds minslice) { timeslice_core(minslice); }
	void abort_timeslice();
	void trigger(int trigid, attotime const &after = attotime::zero);
	void boost_interleave(subseconds timeslice, attotime const &boost_duration) { add_scheduling_quantum(timeslice, boost_duration); }
	void boost_interleave(attotime const &timeslice_time, attotime const &boost_duration) { boost_interleave(timeslice_time.as_subseconds(), boost_duration); }
	void suspend_resume_changed() { m_suspend_changes_pending = true; }

	// timer callback registration
	u32 register_callback(timer_callback &callback);
	void deregister_callback(timer_callback &callback);

	// timers, specified by callback/name; using persistent_timer is preferred
	void synchronize() { m_empty_timer.synchronize(); }

	// pointer to the current callback timer, if live
	timer_instance *callback_timer() const { return m_callback_timer; }

	// debugging
	void dump_timers() const;

	// force immediate exit from the scheduling loop -- used for major state
	// transitions like hard reset or save state restore
	void hard_stop(bool load_after_stop);

	// save state registration
	void register_save(save_manager &save);

private:
	// callbacks
	void presave();
	void postload();

	// execution helpers
	void timeslice_core(subseconds minslice);
	void timeslice_partial();
	void execute_timers(attotime const &basetime);
	void update_first_timer_expire() { if (!m_hard_stopping) m_first_timer_expire.set(m_active_timers_head->m_expire); }
	void update_basetime();
	void hard_stop_complete();
	void rebuild_execute_list();

	// scheduling helpers
	void compute_perfect_interleave();
	void apply_suspend_changes(bool advance);
	void add_scheduling_quantum(subseconds quantum, attotime const &duration);

	// timer instance management
	timer_instance &instance_alloc();
	void instance_reclaim(timer_instance &timer);
	timer_instance &instance_insert(timer_instance &instance);
	timer_instance &instance_remove(timer_instance &instance);

	// internal timers
	void empty_timer(timer_instance const &timer);
	void timed_trigger(timer_instance const &timer);

	// basetime_relative helpers
	attotime const &basetime() const { return m_basetime.absolute_no_update(); }

	// internal state
	running_machine &           m_machine;                  // reference to our machine
	device_execute_interface *  m_executing_device;         // pointer to currently executing device
	device_execute_interface *  m_execute_list;             // list of devices to be executed
	basetime_relative           m_basetime;                 // global basetime; everything moves forward from here
	basetime_relative           m_first_timer_expire;       // time of the first timer expiration

	// timer allocation and management
	timer_instance *            m_active_timers_head;       // head of the list of active timers
	timer_instance              m_active_timers_tail;       // tail of the list, always present
	timer_instance *            m_free_timers;              // simple list of free timers
	timer_callback *            m_registered_callbacks;     // list of registered callbacks
	transient_timer_factory     m_empty_timer;              // empty timer factory
	transient_timer_factory     m_timed_trigger;            // timed trigger factory
	std::vector<std::unique_ptr<timer_instance>> m_allocated_instances;

	// other internal states
	timer_instance *            m_callback_timer;           // pointer to the current callback timer
	attotime                    m_callback_timer_expire_time; // the original expiration time
	bool                        m_suspend_changes_pending;  // suspend/resume changes are pending
	bool                        m_in_timeslice;             // true if we're in a timeslice call
	bool                        m_hard_stopping;            // true if we're trying to exit ASAP
	bool                        m_load_after_stop;          // true if we should load after stopping

	// statistics
#if (COLLECT_SCHEDULER_STATS)
	u64                         m_timeslice = 0;
	u64                         m_timeslice_inner1 = 0;
	u64                         m_timeslice_inner2 = 0;
	u64                         m_timeslice_inner3 = 0;
	u64                         m_execute_timers = 0;
	u64                         m_execute_timers_average = 0;
	u64                         m_update_basetime = 0;
	u64                         m_compute_perfect_interleave = 0;
	u64                         m_apply_suspend_changes = 0;
	u64                         m_add_scheduling_quantum = 0;
	u64                         m_instance_alloc = 0;
	u64                         m_instance_alloc_full = 0;
	u64                         m_instance_insert_head = 0;
	u64                         m_instance_insert_tail = 0;
	u64                         m_instance_insert_middle = 0;
	u64                         m_instance_insert_average = 0;
	u64                         m_instance_remove = 0;
	u64                         m_empty_timer_calls = 0;
	u64                         m_timed_trigger_calls = 0;
#endif

	// scheduling quanta
	static constexpr subseconds MAX_QUANTUM = subseconds::from_hz(10);
	class quantum_slot
	{
	public:
		subseconds              m_actual;                   // actual duration of the quantum
		subseconds              m_requested;                // duration of the requested quantum
		basetime_relative       m_expire;                   // absolute expiration time of this quantum

		void register_save(save_manager &save, int index)
		{
			save.save_item(nullptr, "quantum_slot", "", index, NAME(m_actual));
			save.save_item(nullptr, "quantum_slot", "", index, NAME(m_requested));
			save.save_item(nullptr, "quantum_slot", "", index, NAME(m_expire.m_absolute));
			save.save_item(nullptr, "quantum_slot", "", index, NAME(m_expire.m_relative));
			save.save_item(nullptr, "quantum_slot", "", index, NAME(m_expire.m_base));
		}
	};
	subseconds                  m_quantum_minimum;          // duration of minimum quantum
	u32                         m_quantum_count;            // number of currently active quanta
	quantum_slot                m_quantum_slot[MAX_ACTIVE_QUANTA]; // array of active quanta

	// save data; put this at the end since it's big
	bool                        m_midslice_restore;         // true if we're in a mid-timeslice restore
	s32                         m_save_executing;           // index of executing device at save
	s32                         m_save_icount;              // icount of executing device at save
	subseconds                  m_save_target;              // target subseconds of current slice
	timer_instance_save         m_timer_save[TIMER_SAVE_SLOTS]; // state saving area
};



//**************************************************************************
//  INLINE FUNCTIONS
//**************************************************************************

//-------------------------------------------------
//  form1_callback - wrapper delegate for a form 1
//  style callback
//-------------------------------------------------

inline void timer_expired_delegate::form1_callback(timer_instance const &timer)
{
	reinterpret_cast<timer_expired_delegate_form1 &>(m_sub_delegate)();
}


//-------------------------------------------------
//  form2_callback - wrapper delegate for a form 2
//  style callback
//-------------------------------------------------

template<typename IntType>
inline void timer_expired_delegate::form2_callback(timer_instance const &timer)
{
	reinterpret_cast<timer_expired_delegate_form2<IntType> &>(m_sub_delegate)(IntType(timer.param()));
}


//-------------------------------------------------
//  form3_callback - wrapper delegate for a form 3
//  style callback
//-------------------------------------------------

template<typename IntType>
inline void timer_expired_delegate::form3_callback(timer_instance const &timer)
{
	reinterpret_cast<timer_expired_delegate_form3<IntType> &>(m_sub_delegate)(timer.ptr(), IntType(timer.param()));
}


//-------------------------------------------------
//  form4_callback - wrapper delegate for a form 4
//  style callback
//-------------------------------------------------

template<typename IntType, typename IntType2>
inline void timer_expired_delegate::form4_callback(timer_instance const &timer)
{
	reinterpret_cast<timer_expired_delegate_form4<IntType, IntType2> &>(m_sub_delegate)(IntType(timer.param(0)), IntType2(timer.param(1)));
}


//-------------------------------------------------
//  form5_callback - wrapper delegate for a form 5
//  style callback
//-------------------------------------------------

template<typename IntType, typename IntType2, typename IntType3>
inline void timer_expired_delegate::form5_callback(timer_instance const &timer)
{
	reinterpret_cast<timer_expired_delegate_form5<IntType, IntType2, IntType3> &>(m_sub_delegate)(IntType(timer.param(0)), IntType2(timer.param(1)), IntType3(timer.param(2)));
}


//-------------------------------------------------
//  call_after - create a new timer that will
//  call the callback after a given amount of time
//-------------------------------------------------

inline void transient_timer_factory::call_after(attotime const &duration, u64 param, u64 param2, u64 param3)
{
	scheduler_assert(!duration.is_never());
	scheduler_assert(m_callback.is_initialized());
	m_callback.scheduler().instance_alloc().init_transient(m_callback, duration, false)
		.set_params(param, param2, param3);
}


//-------------------------------------------------
//  call_at - create a new timer that will call
//  the callback at a specific time
//-------------------------------------------------

inline void transient_timer_factory::call_at(attotime const &abstime, u64 param, u64 param2, u64 param3)
{
	scheduler_assert(!abstime.is_never());
	scheduler_assert(m_callback.is_initialized());
	m_callback.scheduler().instance_alloc().init_transient(m_callback, abstime, true)
		.set_params(param, param2, param3);
}



#endif  // MAME_EMU_SCHEDULE_H
