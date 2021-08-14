// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/*************************************************************************

    Sega Z80-3D system

*************************************************************************/

#include "emu.h"
#include "includes/turbo.h"
#include "speaker.h"



/*************************************
 *
 *  Turbo shared state updates
 *
 *************************************/

void turbo_state::update_samples()
{
	// accelerator sounds
	// BSEL == 3 --> off
	// BSEL == 2 --> standard
	// BSEL == 1 --> tunnel
	// BSEL == 0 --> ???
	if (m_bsel == 3 && m_samples->playing(5))
		m_samples->stop(5);
	else if (m_bsel != 3 && !m_samples->playing(5))
		m_samples->start(5, 7, true);
	if (m_samples->playing(5))
		m_samples->set_frequency(5, m_samples->base_frequency(5) * ((m_accel & 0x3f) / 5.25 + 1));
}



/*************************************
 *
 *  Turbo PPI write handlers
 *
 *************************************/

void turbo_state::sound_a_w(uint8_t data)
{
	uint8_t diff = data ^ m_sound_state[0];
	m_sound_state[0] = data;

	/* /CRASH.S: channel 0 */
	if ((diff & 0x01) && !(data & 0x01)) m_samples->start(0, 5);

	// /TRIG1: channel 1
	if ((diff & 0x02) && !(data & 0x02)) m_samples->start(1, 0);

	// /TRIG2: channel 1
	if ((diff & 0x04) && !(data & 0x04)) m_samples->start(1, 1);

	// /TRIG3: channel 1
	if ((diff & 0x08) && !(data & 0x08)) m_samples->start(1, 2);

	// /TRIG4: channel 1
	if ((diff & 0x10) && !(data & 0x10)) m_samples->start(1, 3);

	// OSEL0
	m_osel = (m_osel & 6) | ((data >> 5) & 1);

	// /SLIP: channel 2
	if ((diff & 0x40) && !(data & 0x40)) m_samples->start(2, 4);

	// /CRASH.L: channel 3
	if ((diff & 0x80) && !(data & 0x80)) m_samples->start(3, 5);
}


void turbo_state::sound_b_w(uint8_t data)
{
	uint8_t diff = data ^ m_sound_state[1];
	m_sound_state[1] = data;

	// ACC0-ACC5
	m_accel = data & 0x3f;
	m_tachometer = m_accel;

	// /AMBU: channel 4
	if ((diff & 0x40) && !(data & 0x40) && !m_samples->playing(4)) m_samples->start(4, 8, true);
	if ((diff & 0x40) &&  (data & 0x40)) m_samples->stop(4);

	// /SPIN: channel 2
	if ((diff & 0x80) && !(data & 0x80)) m_samples->start(2, 6);

	// update any samples
	update_samples();
}


void turbo_state::sound_c_w(uint8_t data)
{
	// OSEL1-2
	m_osel = (m_osel & 1) | ((data & 3) << 1);

	// BSEL0-1
	m_bsel = (data >> 2) & 3;

	// SPEED0-3
	m_speed = (data >> 4) & 0x0f;

	// update any samples
	update_samples();
}



/*************************************
 *
 *  Turbo sound hardware description
 *
 *************************************/

static const char *const turbo_sample_names[] =
{
	"*turbo",
	"01",       // 0: Trig1
	"02",       // 1: Trig2
	"03",       // 2: Trig3
	"04",       // 3: Trig4
	"05",       // 4: Screech
	"06",       // 5: Crash
	"skidding", // 6: Spin
	"idle",     // 7: Idle
	"ambulanc", // 8: Ambulance
	nullptr
};


void turbo_state::turbo_samples(machine_config &config)
{
	// this is the cockpit speaker configuration
	SPEAKER(config, "fspeaker", 0.0, 0.0, 1.0);     // front
	SPEAKER(config, "bspeaker",  0.0, 0.0, -0.5);   // back
	SPEAKER(config, "lspeaker", -0.2, 0.0, 1.0);    // left
	SPEAKER(config, "rspeaker", 0.2, 0.0, 1.0);     // right

	SAMPLES(config, m_samples);
	m_samples->set_channels(10);
	m_samples->set_samples_names(turbo_sample_names);

	// channel 0 = CRASH.S -> CRASH.S/SM
	m_samples->add_route(0, "fspeaker", 0.25);

	// channel 1 = TRIG1-4 -> ALARM.M/F/R/L
	m_samples->add_route(1, "fspeaker", 0.25);
	m_samples->add_route(1, "rspeaker", 0.25);
	m_samples->add_route(1, "lspeaker",  0.25);

	// channel 2 = SLIP/SPIN -> SKID.F/R/L/M
	m_samples->add_route(2, "fspeaker", 0.25);
	m_samples->add_route(2, "rspeaker", 0.25);
	m_samples->add_route(2, "lspeaker",  0.25);

	// channel 3 = CRASH.L -> CRASH.L/LM
	m_samples->add_route(3, "bspeaker",  0.25);

	// channel 4 = AMBU -> AMBULANCE/AMBULANCE.M
	m_samples->add_route(4, "fspeaker", 0.25);

	// channel 5 = ACCEL+BSEL -> MYCAR.F/W/M + MYCAR0.F/M + MYCAR1.F/M
	m_samples->add_route(5, "fspeaker", 0.25);
	m_samples->add_route(5, "bspeaker",  0.25);

	// channel 6 = OSEL -> OCAR.F/FM
	m_samples->add_route(6, "fspeaker", 0.25);

	// channel 7 = OSEL -> OCAR.L/LM
	m_samples->add_route(7, "lspeaker",  0.25);

	// channel 8 = OSEL -> OCAR.R/RM
	m_samples->add_route(8, "rspeaker", 0.25);

	// channel 9 = OSEL -> OCAR.W/WM
	m_samples->add_route(9, "bspeaker",  0.25);
}

/*
    Cockpit: CN2 1+2 -> FRONT
             CN2 3+4 -> REAR
             CN2 5+6 -> RIGHT
             CN2 7+8 -> LEFT

    Upright: CN2 1+2 -> UPPER
             CN2 3+4 -> LOWER

    F.OUT = CRASH.S +
            ALARM.F +
            SKID.F +
            OCAR.F +
            MYCAR.F +
            MYCAR0.F +
            MYCAR1.F +
            AMBULACE

    W.OUT = CRASH.L +
            OCAR.W +
            MYCAR.W +
            MYCAR0.W +
            MYCAR1.W +
            SLF

    R.OUT = ALARM.R +
            SKID.R +
            OCAR.R

    L.OUT = ALARM.L +
            SKID.L +
            OCAR.L

    M.OUT = CRASH.SM +
            CRASH.LM +
            SKID.M +
            ALARM.M +
            AMBULACE.M +
            MYCAR.M +
            MYCAR0.M +
            MYCAR1.M +
            OCAR.FM +
            OCAR.LM +
            OCAR.RM +
            OCAR.WM
*/



/*************************************
 *
 *  Subroc 3D PPI write handlers
 *
 *************************************/

void subroc3d_state::sound_a_w(uint8_t data)
{
	m_sound_state[0] = data;

	// DIS0-3 contained in bits 0-3
	// DIR0-2 contained in bits 4-6
}


inline void subroc3d_state::update_volume(int leftchan, uint8_t dis, uint8_t dir)
{
	float volume = (float)(15 - dis) / 16.0f;
	float lvol, rvol;

	// compute the left/right volume from the data
	if (dir != 7)
	{
		lvol = volume * (float)(6 - dir) / 6.0f;
		rvol = volume * (float)dir / 6.0f;
	}
	else
		lvol = rvol = 0;

	// if the sample is playing, adjust it
	m_samples->set_volume(leftchan + 0, lvol);
	m_samples->set_volume(leftchan + 1, rvol);
}


void subroc3d_state::sound_b_w(uint8_t data)
{
	uint8_t diff = data ^ m_sound_state[1];
	m_sound_state[1] = data;

	// bit 0 latches direction/volume for missile
	if ((diff & 0x01) && (data & 0x01))
	{
		m_mdis = m_sound_state[0] & 0x0f;
		m_mdir = (m_sound_state[0] >> 4) & 0x07;
		if (!m_samples->playing(0))
		{
			m_samples->start(0, 0, true);
			m_samples->start(1, 0, true);
		}
		update_volume(0, m_mdis, m_mdir);
	}

	// bit 1 latches direction/volume for torpedo
	if ((diff & 0x02) && (data & 0x02))
	{
		m_tdis = m_sound_state[0] & 0x0f;
		m_tdir = (m_sound_state[0] >> 4) & 0x07;
		if (!m_samples->playing(2))
		{
			m_samples->start(2, 1, true);
			m_samples->start(3, 1, true);
		}
		update_volume(2, m_tdis, m_tdir);
	}

	// bit 2 latches direction/volume for fighter
	if ((diff & 0x04) && (data & 0x04))
	{
		m_fdis = m_sound_state[0] & 0x0f;
		m_fdir = (m_sound_state[0] >> 4) & 0x07;
		if (!m_samples->playing(4))
		{
			m_samples->start(4, 2, true);
			m_samples->start(5, 2, true);
		}
		update_volume(4, m_fdis, m_fdir);
	}

	// bit 3 latches direction/volume for hit
	if ((diff & 0x08) && (data & 0x08))
	{
		m_hdis = m_sound_state[0] & 0x0f;
		m_hdir = (m_sound_state[0] >> 4) & 0x07;
		update_volume(6, m_hdis, m_hdir);
	}
}


void subroc3d_state::sound_c_w(uint8_t data)
{
	uint8_t diff = data ^ m_sound_state[2];
	m_sound_state[2] = data;

	// /FIRE TRIG
	// FIRE SELECT
	if ((diff & 0x01) && (data & 0x01))
		m_samples->start(8, (data & 0x02) ? 6 : 5);

	// /SHIP EXP TRIG -> MY SHIP EXP: channel 9
	if ((diff & 0x04) && (data & 0x04))
		m_samples->start(9, 7);

	// /HIT TRIG -> HIT.L/R: channels 6+7
	if ((diff & 0x08) && (data & 0x08))
	{
		m_samples->start(6, (m_sound_state[0] & 0x80) ? 4 : 3);
		m_samples->start(7, (m_sound_state[0] & 0x80) ? 4 : 3);
	}

	// /ALARM TRIG -> ALARM.M: channel 10
	// ALARM SELECT
	if ((diff & 0x10) && (data & 0x10))
		m_samples->start(10, (data & 0x20) ? 10 : 9);

	// /PROLOGUE
	if (!m_samples->playing(11))
		m_samples->start(11, 8, true);
	m_samples->set_volume(11, (data & 0x40) ? 0 : 1.0);

	// /GAME START
	machine().sound().system_mute(data & 0x80);
}



/*************************************
 *
 *  Subroc 3D sound hardware description
 *
 *************************************/

static const char *const subroc3d_sample_names[] =
{
	"*subroc3d",
	"01",   //  0: enemy missile
	"02",   //  1: enemy torpedo
	"03",   //  2: enemy fighter
	"04",   //  3: explosion in sky
	"05",   //  4: explosion on sea
	"06",   //  5: missile shoot
	"07",   //  6: torpedo shoot
	"08",   //  7: my ship expl
	"09",   //  8: prolog sound
	"11",   //  9: alarm 0
	"12",   // 10: alarm 1
	nullptr
};

void subroc3d_state::subroc3d_samples(machine_config &config)
{
	SPEAKER(config, "lspeaker").front_left();
	SPEAKER(config, "rspeaker").front_right();

	SAMPLES(config, m_samples);
	m_samples->set_channels(12);
	m_samples->set_samples_names(subroc3d_sample_names);

	// MISSILE in channels 0 and 1
	m_samples->add_route(0, "lspeaker", 0.25);
	m_samples->add_route(1, "rspeaker", 0.25);

	// TORPEDO in channels 2 and 3
	m_samples->add_route(2, "lspeaker", 0.25);
	m_samples->add_route(3, "rspeaker", 0.25);

	// FIGHTER in channels 4 and 5
	m_samples->add_route(4, "lspeaker", 0.25);
	m_samples->add_route(5, "rspeaker", 0.25);

	// HIT in channels 6 and 7
	m_samples->add_route(6, "lspeaker", 0.25);
	m_samples->add_route(7, "rspeaker", 0.25);

	// FIRE sound in channel 8
	m_samples->add_route(8, "lspeaker", 0.25);
	m_samples->add_route(8, "rspeaker", 0.25);

	// SHIP EXP sound in channel 9
	m_samples->add_route(9, "lspeaker", 0.25);
	m_samples->add_route(9, "rspeaker", 0.25);

	// ALARM TRIG sound in channel 10
	m_samples->add_route(10, "lspeaker", 0.25);
	m_samples->add_route(10, "rspeaker", 0.25);

	// PROLOGUE sound in channel 11
	m_samples->add_route(11, "lspeaker", 0.25);
	m_samples->add_route(11, "rspeaker", 0.25);
}



/*************************************
 *
 *  Buck Rogers PPI write handlers
 *
 *************************************/

void buckrog_state::update_samples()
{
	// accelerator sounds
	if (m_samples->playing(5))
		m_samples->set_frequency(5, m_samples->base_frequency(5) * (m_myship / 100.25 + 1));
}


void buckrog_state::sound_a_w(uint8_t data)
{
	uint8_t diff = data ^ m_sound_state[0];
	m_sound_state[0] = data;

	// clock HIT DIS from bits 0-2
	if ((diff & 0x10) && (data & 0x10))
		m_samples->set_volume(3, (float)(/*7 - */(data & 7)) / 7.0f);

	// clock ACC from bits 0-3
	if ((diff & 0x20) && (data & 0x20))
	{
		m_myship = data & 0x0f;
		update_samples();
	}

	// /ALARM0: channel 0
	if ((diff & 0x40) && !(data & 0x40)) m_samples->start(0, 0);

	// /ALARM1: channel 0
	if ((diff & 0x80) && !(data & 0x80)) m_samples->start(0, 1);
}


void buckrog_state::sound_b_w(uint8_t data)
{
	uint8_t diff = data ^ m_sound_state[1];
	m_sound_state[1] = data;

	// /ALARM3: channel 0
	if ((diff & 0x01) && !(data & 0x01)) m_samples->start(0, 2);

	// /ALARM4: channel 0
	if ((diff & 0x02) && !(data & 0x02)) m_samples->start(0, 3);

	// /FIRE: channel 1
	if ((diff & 0x04) && !(data & 0x04)) m_samples->start(1, 5);

	// /EXP: channel 2
	if ((diff & 0x08) && !(data & 0x08)) m_samples->start(2, 4);

	// /HIT: channel 3
	if ((diff & 0x10) && !(data & 0x10))
	{
		m_samples->start(3, 7);
		update_samples();
	}

	// /REBOUND: channel 4
	if ((diff & 0x20) && !(data & 0x20)) m_samples->start(4, 6);

	// SHIP: channel 5
	if ((diff & 0x40) &&  (data & 0x40) && !m_samples->playing(5))
	{
		m_samples->start(5, 8, true);
		update_samples();
	}
	if ((diff & 0x40) && !(data & 0x40) &&  m_samples->playing(5)) m_samples->stop(5);

	// GAME ON
	machine().sound().system_mute(!BIT(data, 7));
}



/*************************************
 *
 *  Sound interfaces
 *
 *************************************/

static const char *const buckrog_sample_names[]=
{
	"*buckrog",
	"alarm0",   // 0
	"alarm1",   // 1
	"alarm2",   // 2
	"alarm3",   // 3
	"exp",      // 4
	"fire",     // 5
	"rebound",  // 6
	"hit",      // 7
	"shipsnd1", // 8
	"shipsnd2", // 9
	"shipsnd3", // 10
	nullptr
};


void buckrog_state::buckrog_samples(machine_config &config)
{
	SPEAKER(config, "mono").front_center();
	SAMPLES(config, m_samples);
	m_samples->set_channels(6);
	m_samples->set_samples_names(buckrog_sample_names);
	m_samples->add_route(ALL_OUTPUTS, "mono", 0.25);
}
