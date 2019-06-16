// license:BSD-3-Clause
// copyright-holders:R. Belmont
/****************************************************************************

    drivers/ncd68k.cpp
    NCD 16" monochrome X terminal
    NCD 17" color X terminal
    NCD 19" monochrome X terminal

    Hardware:
        - MC68020 CPU, optional FPU, no MMU
        - 2681 DUART (Logitech serial mouse)
        - AMD LANCE Ethernet controller
        - Bt478 RAMDAC
        - MC6805 keyboard and NVRAM handler

        68020 IRQs: 1 = software-triggered IRQ, 2 = keyboard, 3 = LANCE, 4 = DUART, 5 = vblank
        68000     : 1 = DUART, 2 = keyboard, 3 = LANCE, 7 = vblank

        6805 port assignments:
        A0 - IN  - keyboard data in
        A1 - OUT - keyboard data out
        A2 - OUT - keyboard clock out
        A3 - OUT - chip select on the 93C46 EEPROM
        A4 - OUT - when clear port B reads as the mailslot from the 68020
        A5 - IN  - set if the 68020 has nothing new for us
        A6 - OUT - rising edge latches port B to the 68020 mailslot and raises the IRQ
        A7 - IN  - set if the 68020 hasn't yet read our last transmission

        B0 - OUT - SK line on 93C46 EEPROM
        B1 - OUT - Data In line on 93C46 EEPROM
        B2 - IN  - Data Out line on 93C46 EEPROM
        B3-B7 unused except '020 mailslot interface

        C0-C3 = speaker (4-bit DAC?)  The 6805 timer is used to control this.

        IRQ in = keyboard clock in

****************************************************************************/

/*
 * WIP status
 *   - ncd16   boots, QLC/BERT test failure
 *   - ncd17c  nvram timeout or checksum failure
 *   - NCD19   boots, tries to contact network (timeout)
 *
 * DUART IP2 is set when the 68k mailbox is full (inverse of mcu A5), probably
 * both of these are tied to a latch with a flip-flop
 *
 * TODO
 *   - tidy latches
 *   - fix keyboard
 *   - sound
 *   - refactor
 */
#include "emu.h"

// processors
#include "cpu/m68000/m68000.h"
#include "cpu/m6805/m68705.h"

// i/o devices
#include "machine/mc68681.h"
#include "machine/am79c90.h"
#include "machine/eepromser.h"

// busses and connectors
#include "bus/pc_kbd/pc_kbdc.h"
#include "bus/pc_kbd/keyboards.h"
#include "bus/rs232/rs232.h"
#include "bus/rs232/hlemouse.h"

// video and audio
#include "screen.h"

#define LOG_GENERAL (1U << 0)
#define LOG_MCU     (1U << 1)

//#define VERBOSE (LOG_GENERAL|LOG_MCU)
#include "logmacro.h"

class ncd_68k_state : public driver_device
{
public:
	ncd_68k_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag)
		, m_maincpu(*this, "maincpu")
		, m_mcu(*this, "mcu")
		, m_screen(*this, "screen")
		, m_mainram(*this, "mainram")
		, m_vram(*this, "vram")
		, m_mainram16(*this, "mainram16")
		, m_vram16(*this, "vram16")
		, m_duart(*this, "duart")
		, m_lance(*this, "am79c90")
		, m_kbd_con(*this, "kbd_con")
		, m_serial(*this, "serial%u", 0U)
		, m_eeprom(*this, "eeprom")
	{
	}

	void ncd_16(machine_config &config);
	void ncd_17c(machine_config &config);
	void ncd_19(machine_config &config);

protected:
	virtual void machine_reset() override;

private:
	void ncd_16_map(address_map &map);
	void ncd_17c_map(address_map &map);
	void ncd_19_map(address_map &map);

	void common(machine_config &config);

	u32 screen_update_16(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
	u32 screen_update_17c(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
	u32 screen_update_19(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);

	DECLARE_READ16_MEMBER(lance17c_dma_r);
	DECLARE_WRITE16_MEMBER(lance17c_dma_w);
	DECLARE_READ16_MEMBER(lance19_dma_r);
	DECLARE_WRITE16_MEMBER(lance19_dma_w);
	DECLARE_READ16_MEMBER(lance16_dma_r);
	DECLARE_WRITE16_MEMBER(lance16_dma_w);
	DECLARE_WRITE32_MEMBER(bt478_palette_w);
	u8 mcu_r();
	void mcu_w(u8 data);
	u8 mcu_status_r();
	void irq_w(u8 data);

	u8 mcu_porta_r();
	u8 mcu_portb_r();
	void mcu_porta_w(u8 data);
	void mcu_portb_w(u8 data);
	void mcu_portc_w(u8 data);

	required_device<m68000_base_device> m_maincpu;
	required_device<m6805_hmos_device> m_mcu;
	required_device<screen_device> m_screen;
	optional_shared_ptr<u32> m_mainram;
	optional_shared_ptr<u32> m_vram;
	optional_shared_ptr<u16> m_mainram16;
	optional_shared_ptr<u16> m_vram16;
	required_device<scn2681_device> m_duart;
	required_device<am7990_device> m_lance;
	required_device<pc_kbdc_device> m_kbd_con;
	required_device_array<rs232_port_device, 2> m_serial;
	required_device<eeprom_serial_93c46_16bit_device> m_eeprom;

	u32 m_palette[256];
	u8 m_r, m_g, m_b, m_entry, m_stage;
	u8 m_portc, m_to_68k, m_from_68k;
	u8 m_porta_in, m_porta_out;
	u8 m_portb_in, m_portb_out;
};

void ncd_68k_state::machine_reset()
{
	m_entry = 0;
	m_stage = 0;
	m_r = m_g = m_b = 0;
	m_porta_in = m_porta_out = m_portb_in = m_portb_out = m_portc = 0;
	m_to_68k = m_from_68k = 0;

	m_porta_in |= 0x20;
}

u32 ncd_68k_state::screen_update_16(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	static constexpr u32 palette[2] = { 0, 0xffffff };
	u8 const *const vram = (u8 *)m_vram16.target();

	for (int y = 0; y < 1024; y++)
	{
		u32 *scanline = &bitmap.pix32(y);
		for (int x = 0; x < 1024 / 8; x++)
		{
			u8 const pixels = vram[(y * (1024 / 8)) + (x ^ 1)];

			for (int b = 0; b < 8; b++)
				*scanline++ = palette[BIT(pixels, 7 - b)];
		}
	}
	return 0;
}

u32 ncd_68k_state::screen_update_17c(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	u32 *scanline;
	int x, y;
	u8 pixels;
	u8 *vram = (u8 *)m_mainram.target();

	for (y = 0; y < 768; y++)
	{
		scanline = &bitmap.pix32(y);
		for (x = 0; x < 1024; x++)
		{
			pixels = vram[(y * 1024) + (BYTE4_XOR_BE(x))];
			*scanline++ = m_palette[pixels];
		}
	}

	return 0;
}

u32 ncd_68k_state::screen_update_19(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	static constexpr u32 palette[2] = { 0, 0xffffff };
	u8 const *const vram = (u8 *)m_vram.target();

	for (int y = 0; y < 1024; y++)
	{
		u32 *scanline = &bitmap.pix32(y);
		for (int x = 0; x < 1280/8; x++)
		{
			u8 const pixels = vram[(y * (2048/8)) + (BYTE4_XOR_BE(x))];

			for (int b = 0; b < 8; b++)
				*scanline++ = palette[BIT(pixels, 7 - b)];
		}
	}

	return 0;
}

void ncd_68k_state::ncd_16_map(address_map &map)
{
	map(0x000000, 0x0bffff).rom().region("maincpu", 0);
	map(0x0c0000, 0x0c0001).rw(FUNC(ncd_68k_state::mcu_r), FUNC(ncd_68k_state::mcu_w)).umask16(0x00ff);
	map(0x0e0000, 0x0e003f).rw(m_duart, FUNC(scn2681_device::read), FUNC(scn2681_device::write)).umask16(0xff00);
	map(0x100000, 0x100003).rw(m_lance, FUNC(am79c90_device::regs_r), FUNC(am79c90_device::regs_w));
	map(0x200000, 0x21ffff).ram().share("vram16");
	map(0x380000, 0x5fffff).ram().share("mainram16");

	//map(0xa00000, 0xa002ff).ram(); // QLC?
	//map(0xbf0000, 0xbf000f).ram(); // BERT?
}

void ncd_68k_state::ncd_17c_map(address_map &map)
{
	map(0x00000000, 0x000bffff).rom().region("maincpu", 0);
	map(0x001c0000, 0x001c0003).rw(FUNC(ncd_68k_state::mcu_r), FUNC(ncd_68k_state::mcu_w)).umask32(0xff000000);
	map(0x001c8000, 0x001c803f).rw(m_duart, FUNC(scn2681_device::read), FUNC(scn2681_device::write)).umask32(0xff000000);
	map(0x001d0000, 0x001d0003).w(FUNC(ncd_68k_state::bt478_palette_w));
	map(0x001d8000, 0x001d8003).rw(FUNC(ncd_68k_state::mcu_status_r), FUNC(ncd_68k_state::irq_w)).umask32(0xff000000);
	map(0x00200000, 0x00200003).rw(m_lance, FUNC(am79c90_device::regs_r), FUNC(am79c90_device::regs_w)).umask32(0xffffffff);
	map(0x01000000, 0x02ffffff).ram();
	map(0x03000000, 0x03ffffff).ram().share("mainram");
}

void ncd_68k_state::ncd_19_map(address_map &map)
{
	map(0x00000000, 0x0000ffff).rom().region("maincpu", 0);
	map(0x001c0000, 0x001c0003).rw(FUNC(ncd_68k_state::mcu_r), FUNC(ncd_68k_state::mcu_w)).umask32(0xff000000);
	map(0x001d8000, 0x001d8003).rw(FUNC(ncd_68k_state::mcu_status_r), FUNC(ncd_68k_state::irq_w)).umask32(0xff000000);
	map(0x001e0000, 0x001e003f).rw(m_duart, FUNC(scn2681_device::read), FUNC(scn2681_device::write)).umask32(0xff000000);
	map(0x00200000, 0x00200003).rw(m_lance, FUNC(am79c90_device::regs_r), FUNC(am79c90_device::regs_w)).umask32(0xffffffff);
	map(0x00400000, 0x0043ffff).ram().share("vram");
	map(0x00800000, 0x00ffffff).ram().share("mainram");
}

u8 ncd_68k_state::mcu_porta_r()
{
	return m_porta_in;
}

u8 ncd_68k_state::mcu_portb_r()
{
	// read from the mailbox or from the port B latch
	if (!BIT(m_porta_out, 4))
	{
		// FIXME: ncd19 requires this, but ncd17c fails with an nvram timeout
		m_porta_in |= 0x20;
		m_duart->ip2_w(0);

		return m_from_68k;
	}
	else
		return m_portb_in;
}

void ncd_68k_state::mcu_porta_w(u8 data)
{
	if ((data & 0x40) && !(m_porta_out & 0x40))
	{
		m_to_68k = m_portb_out;
		m_porta_in |= 0x80;

		m_maincpu->set_input_line(M68K_IRQ_2, ASSERT_LINE);
	}

	m_kbd_con->data_write_from_mb(BIT(data, 1));
	m_kbd_con->clock_write_from_mb(BIT(data, 2));
	m_eeprom->cs_write(BIT(data, 3));

	m_porta_out = data;
}

void ncd_68k_state::mcu_portb_w(u8 data)
{
	// check if eeprom chip select asserted
	if (BIT(m_porta_out, 3))
	{
		m_eeprom->clk_write(BIT(data, 0));
		m_eeprom->di_write(BIT(data, 1));
	}

	// TODO: what is bit 7 used as an output for?

	m_portb_out = data;
}

void ncd_68k_state::mcu_portc_w(u8 data)
{
	m_portc = data;
}

u8 ncd_68k_state::mcu_r()
{
	LOGMASKED(LOG_MCU, "mcu_r 0x%02x\n", m_to_68k);

	m_porta_in &= ~0x80;
	m_maincpu->set_input_line(M68K_IRQ_2, CLEAR_LINE);

	return m_to_68k;
}

void ncd_68k_state::mcu_w(u8 data)
{
	LOGMASKED(LOG_MCU, "mcu_w 0x%02x (%s)\n", data, machine().describe_context());

	m_from_68k = data;

	m_porta_in &= ~0x20;
	m_duart->ip2_w(1);
}

u8 ncd_68k_state::mcu_status_r()
{
	u8 rv = 0;
	if (!(m_porta_in & 0x20))
	{
		rv |= 0x01;
	}
	if (m_porta_in & 0x80)
	{
		rv |= 0x02;
	}

	LOGMASKED(LOG_MCU, "mcu_status_r 0x%02x\n", rv);

	return rv;
}

void ncd_68k_state::irq_w(u8 data)
{
	LOGMASKED(LOG_MCU, "irq_w %d (%s)\n", data, machine().describe_context());

	m_maincpu->set_input_line(M68K_IRQ_1, BIT(data, 7));
}

READ16_MEMBER(ncd_68k_state::lance16_dma_r)
{
	//printf("lance16_r @ %x\n", offset);
	if (offset < 0x380000)
	{
		logerror("ncd17c.cpp: DMA target %08x not handled!", offset);
		return 0;
	}

	offset -= 0x380000;
	return m_mainram16.target()[offset >> 1];
}

WRITE16_MEMBER(ncd_68k_state::lance16_dma_w)
{
	//printf("lance16_w: %04x @ %x\n", data, offset);
	if (offset < 0x380000)
	{
		logerror("ncd17c.cpp: DMA target %08x not handled!", offset);
		return;
	}

	offset -= 0x380000;
	COMBINE_DATA(&m_mainram16.target()[offset >> 1]);
}

READ16_MEMBER(ncd_68k_state::lance17c_dma_r)
{
	u32 const data = m_mainram.target()[offset >> 2];

	return (offset & 2) ? u16(data) : u16(data >> 16);
}

WRITE16_MEMBER(ncd_68k_state::lance17c_dma_w)
{
	u32 const existing = m_mainram.target()[offset >> 2];

	if (offset & 2)
		m_mainram.target()[offset >> 2] = (existing & (0xffff0000U | ~mem_mask)) | (data & mem_mask);
	else
		m_mainram.target()[offset >> 2] = (existing & (0x0000ffffU | ~(mem_mask << 16))) | ((data & mem_mask) << 16);
}

READ16_MEMBER(ncd_68k_state::lance19_dma_r)
{
	if (offset < 0x800000)
	{
		fatalerror("ncd17c.cpp: DMA target %08x not handled!", offset);
	}

	offset &= 0x7fffff;
	u32 const data = m_mainram.target()[offset >> 2];

	return (offset & 2) ? u16(data) : u16(data >> 16);
}

WRITE16_MEMBER(ncd_68k_state::lance19_dma_w)
{
	if (offset < 0x800000)
	{
		fatalerror("ncd17c.cpp: DMA target %08x not handled!", offset);
	}

	offset &= 0x7fffff;
	u32 const existing = m_mainram.target()[offset >> 2];

	if (offset & 2)
		m_mainram.target()[offset >> 2] = (existing & (0xffff0000U | ~mem_mask)) | (data & mem_mask);
	else
		m_mainram.target()[offset >> 2] = (existing & (0x0000ffffU | ~(mem_mask << 16))) | ((data & mem_mask) << 16);
}

WRITE32_MEMBER(ncd_68k_state::bt478_palette_w)
{
	if (mem_mask & 0xff000000)
	{
		m_entry = data >> 24;
		m_stage = 0;
		m_r = m_g = m_b = 0;
	}
	else if (mem_mask & 0x00ff0000)
	{
		switch (m_stage)
		{
			case 0:
				m_r = (data>>16) & 0xff;
				m_stage++;
				break;

			case 1:
				m_g = (data>>16) & 0xff;
				m_stage++;
				break;

			case 2:
				m_b = (data>>16) & 0xff;
				m_palette[m_entry] = rgb_t(m_r, m_g, m_b);
				//printf("palette[%d] = RGB(%02x, %02x, %02x)\n", m_entry, m_r, m_g, m_b);
				m_entry++;
				m_entry &= 0xff;
				m_stage = 0;
				m_r = m_g = m_b = 0;
				break;
		}
	}
	else if (mem_mask & 0x0000ff00)
	{
		// pixel mask register, unsure if this is used yet
	}
}

void ncd_68k_state::ncd_16(machine_config &config)
{
	// basic machine hardware
	M68000(config, m_maincpu, 12.5_MHz_XTAL);
	m_maincpu->set_addrmap(AS_PROGRAM, &ncd_68k_state::ncd_16_map);

	// duart
	SCN2681(config, m_duart, 3.6864_MHz_XTAL);
	m_duart->irq_cb().set_inputline(m_maincpu, M68K_IRQ_4);

	// keyboard controller
	M68705P3(config, m_mcu, 3'750'000);

	// ethernet
	AM7990(config, m_lance);
	m_lance->intr_out().set_inputline(m_maincpu, M68K_IRQ_3).invert();
	m_lance->dma_in().set(FUNC(ncd_68k_state::lance16_dma_r));
	m_lance->dma_out().set(FUNC(ncd_68k_state::lance16_dma_w));

	// 124.652 MHz dot clock generated by DP8530; 82.88 kHz horizontal, 70 Hz vertical
	SCREEN(config, m_screen, SCREEN_TYPE_RASTER);
	m_screen->set_raw(124'652'000, 1504, 0, 1024, 1184, 0, 1024);
	m_screen->set_screen_update(FUNC(ncd_68k_state::screen_update_16));
	m_screen->screen_vblank().set_inputline(m_maincpu, M68K_IRQ_5, HOLD_LINE);

	common(config);
}

void ncd_68k_state::ncd_17c(machine_config &config)
{
	// basic machine hardware
	M68020(config, m_maincpu, 20000000);
	m_maincpu->set_addrmap(AS_PROGRAM, &ncd_68k_state::ncd_17c_map);

	// duart
	SCN2681(config, m_duart, 77.4144_MHz_XTAL / 21);
	m_duart->irq_cb().set_inputline(m_maincpu, M68K_IRQ_4);

	// keyboard controller
	M6805P2(config, m_mcu, 3'750'000);

	// ethernet
	AM7990(config, m_lance);
	m_lance->intr_out().set_inputline(m_maincpu, M68K_IRQ_3).invert();
	m_lance->dma_in().set(FUNC(ncd_68k_state::lance17c_dma_r));
	m_lance->dma_out().set(FUNC(ncd_68k_state::lance17c_dma_w));

	// 56.260 kHz horizontal, 70.06 Hz vertical
	SCREEN(config, m_screen, SCREEN_TYPE_RASTER);
	m_screen->set_raw(77.4144_MHz_XTAL, 1376, 0, 1024, 803, 0, 768);
	m_screen->set_screen_update(FUNC(ncd_68k_state::screen_update_17c));
	m_screen->screen_vblank().set_inputline(m_maincpu, M68K_IRQ_5, HOLD_LINE);

	common(config);
}

void ncd_68k_state::ncd_19(machine_config &config)
{
	// basic machine hardware
	M68020(config, m_maincpu, 15_MHz_XTAL);
	m_maincpu->set_addrmap(AS_PROGRAM, &ncd_68k_state::ncd_19_map);

	// duart
	SCN2681(config, m_duart, 3.6864_MHz_XTAL);
	m_duart->irq_cb().set_inputline(m_maincpu, M68K_IRQ_4);

	// keyboard controller
	M6805P2(config, m_mcu, 15_MHz_XTAL / 4);

	// ethernet
	AM7990(config, m_lance);
	m_lance->intr_out().set_inputline(m_maincpu, M68K_IRQ_3).invert();
	m_lance->dma_in().set(FUNC(ncd_68k_state::lance19_dma_r));
	m_lance->dma_out().set(FUNC(ncd_68k_state::lance19_dma_w));

	// 128 MHz dot clock generated by DP8530; 74.074 kHz horizontal, 70.1459 Hz vertical
	SCREEN(config, m_screen, SCREEN_TYPE_RASTER);
	m_screen->set_raw(16_MHz_XTAL * 8, 1728, 0, 1280, 1056, 0, 1024);
	m_screen->set_screen_update(FUNC(ncd_68k_state::screen_update_19));
	m_screen->screen_vblank().set_inputline(m_maincpu, M68K_IRQ_5, HOLD_LINE);

	common(config);
}

void ncd_68k_state::common(machine_config &config)
{
	// HACK: this makes the ncd16 and ncd19 keyboard work
	config.m_perfect_cpu_quantum = subtag("mcu");

	// mcu ports
	m_mcu->porta_w().set(FUNC(ncd_68k_state::mcu_porta_w));
	m_mcu->portb_w().set(FUNC(ncd_68k_state::mcu_portb_w));
	m_mcu->portc_w().set(FUNC(ncd_68k_state::mcu_portc_w));

	m_mcu->porta_r().set(FUNC(ncd_68k_state::mcu_porta_r));
	m_mcu->portb_r().set(FUNC(ncd_68k_state::mcu_portb_r));

	// keyboard connector
	PC_KBDC(config, m_kbd_con, 0);
	m_kbd_con->out_clock_cb().set_inputline(m_mcu, M6805_IRQ_LINE).invert();
	m_kbd_con->out_data_cb().set(
		[this](int state)
		{
			if (state)
				m_porta_in |= 0x01;
			else
				m_porta_in &= ~0x01;
		});

	// keyboard port
	pc_kbdc_slot_device &kbd(PC_KBDC_SLOT(config, "kbd", pc_at_keyboards, STR_KBD_MICROSOFT_NATURAL));
	kbd.set_pc_kbdc_slot(m_kbd_con);

	// mouse and auxiliary ports
	RS232_PORT(config, m_serial[0],
		[](device_slot_interface &device)
		{
			default_rs232_devices(device);
			device.option_add("mouse", LOGITECH_HLE_SERIAL_MOUSE);
		},
		"mouse");
	RS232_PORT(config, m_serial[1], default_rs232_devices, nullptr);

	// duart outputs
	m_duart->a_tx_cb().set(m_serial[0], FUNC(rs232_port_device::write_txd));
	m_duart->b_tx_cb().set(m_serial[1], FUNC(rs232_port_device::write_txd));
	m_duart->outport_cb().set(
		[this](u8 data)
		{
			m_serial[0]->write_rts(BIT(data, 0));
			m_serial[1]->write_rts(BIT(data, 1));
			m_serial[0]->write_dtr(BIT(data, 2));
			m_serial[1]->write_dtr(BIT(data, 3));

			// TODO: bit 4 - usually set
			// TODO: bit 5 - usually clear
			// TODO: bit 6 - usually set
			// TODO: bit 7 - set/cleared continuously
		});

	// duart inputs
	// FIXME: rts/dsr external loopback test fails - dsr might not be correct?
	m_serial[0]->rxd_handler().set(m_duart, FUNC(scn2681_device::rx_a_w));
	m_serial[0]->cts_handler().set(m_duart, FUNC(scn2681_device::ip0_w));
	m_serial[0]->dsr_handler().set(m_duart, FUNC(scn2681_device::ip3_w));
	m_serial[0]->dcd_handler().set(m_duart, FUNC(scn2681_device::ip4_w));

	m_serial[1]->rxd_handler().set(m_duart, FUNC(scn2681_device::rx_b_w));
	m_serial[1]->cts_handler().set(m_duart, FUNC(scn2681_device::ip1_w));
	m_serial[1]->dsr_handler().set(m_duart, FUNC(scn2681_device::ip5_w));
	m_serial[1]->dcd_handler().set(m_duart, FUNC(scn2681_device::ip6_w));

	// eeprom
	EEPROM_93C46_16BIT(config, m_eeprom);
	m_eeprom->do_callback().set(
		[this](int state)
		{
			if (state)
				m_portb_in |= 0x04;
			else
				m_portb_in &= ~0x04;
		});
}

static INPUT_PORTS_START(ncd_68k)
INPUT_PORTS_END

ROM_START(ncd16)
	ROM_REGION16_BE(0xc0000, "maincpu", 0)
	ROM_LOAD16_BYTE("b0e.u3",  0x000000, 0x020000, CRC(8996f939) SHA1(8d6c5649fa927ee16b8632c2f2949997014346d0))
	ROM_LOAD16_BYTE("b0o.u14", 0x000001, 0x020000, CRC(3143e82c) SHA1(37b917304705a450b3272945d4cff851afb97dbe))
	ROM_LOAD16_BYTE("b1e.u2",  0x040000, 0x020000, CRC(82712624) SHA1(7e2ff025bf4e9638bdd58b99dc4dabd86c0d21ee))
	ROM_LOAD16_BYTE("b1o.u13", 0x040001, 0x020000, CRC(432aaa02) SHA1(960291a9b692449e4e787b88528c83a37b3562f2))
	ROM_LOAD16_BYTE("b2e.u1",  0x080000, 0x020000, CRC(290116df) SHA1(1042764eb5f308046c0f019f31df457d360c9db1))
	ROM_LOAD16_BYTE("b2o.u12", 0x080001, 0x020000, CRC(2b364281) SHA1(63c2b3093eee6d40f405ead911843ebeb5193b7a))

	ROM_REGION16_LE(0x80, "eeprom", 0)
	ROM_LOAD("93c46_0000a76f002b.u17", 0x00, 0x80, CRC(656c8444) SHA1(162a71e2d91a104f4251beb17a4a53f24d1e5e03))

	ROM_REGION(0x800, "mcu", 0)
	ROM_LOAD("4903001_v2.00_key_scan.u10", 0x000, 0x800, CRC(acec6140) SHA1(003a27cb22652d37b3be05d4ad4e924fc6c5c8de))
ROM_END

ROM_START(ncd17c)
	ROM_REGION32_BE(0xc0000, "maincpu", 0)
	ROM_LOAD16_BYTE("ncd17c_v2.2.1_b0e.u3",  0x000000, 0x020000, CRC(c47648af) SHA1(563e17ea8f5c9d418fd81f1e797a226937c0e187))
	ROM_LOAD16_BYTE("ncd17c_v2.2.1_b0o.u14", 0x000001, 0x020000, CRC(b6a8c3ca) SHA1(f02e33d88861ebcb402fb554719c1cb072a5fd14))
	ROM_LOAD16_BYTE("ncd17c_v2.2.1_b1e.u2",  0x040000, 0x020000, CRC(25500987) SHA1(cebdf07c69f1c783a67b92d6efdfdd7067dc910f))
	ROM_LOAD16_BYTE("ncd17c_v2.2.1_b1o.u13", 0x040001, 0x020000, CRC(a5d5ab8a) SHA1(51ddb7020abd5f83224bff48eab254375e9d27f9))
	ROM_LOAD16_BYTE("ncd17c_v2.2.1_b2e.u1",  0x080000, 0x020000, CRC(390dac65) SHA1(3f9c886433dff87847135b8f3d8e8ead75d3abf3))
	ROM_LOAD16_BYTE("ncd17c_v2.2.1_b2o.u12", 0x080001, 0x020000, CRC(2e5ebfaa) SHA1(d222c6cc743046a1c1dec1829c24fa918a54849d))

	ROM_REGION16_LE(0x80, "eeprom", 0)
	ROM_LOAD("93c46_0000a7103f0f.u17", 0x00, 0x80, CRC(f7515683) SHA1(3c5796d4fc1e18db8d12b5a4758f083f9dac3f36))

	ROM_REGION(0x800, "mcu", 0)
	ROM_LOAD("ncd4200005.u18", 0x000, 0x800, CRC(075c3746) SHA1(6954cfab5141138df975f1b15d2c8e08d4d203c1))
ROM_END

ROM_START(ncd19)
	ROM_REGION32_BE(0x10000, "maincpu", 0)
	ROM_LOAD16_BYTE("ncd19_v2.1.1_e.u3",  0x000000, 0x008000, CRC(28786528) SHA1(8f4ad6a593c55cce0477169132ecf38577086f4e))
	ROM_LOAD16_BYTE("ncd19_v2.1.1_o.u14", 0x000001, 0x008000, CRC(aeefbcf1) SHA1(0c28426d0ae7c18de02daee7d340c17dc461e7f4))

	ROM_REGION16_LE(0x80, "eeprom", 0)
	ROM_LOAD("93c46_0000a71028bd.u17", 0x00, 0x80, CRC(c2cd34fe) SHA1(77bbbc46c717b15c4a075511e50dfb26cca74029))

	ROM_REGION(0x800, "mcu", 0)
	ROM_LOAD("ncd4200005.u18", 0x000, 0x800, CRC(075c3746) SHA1(6954cfab5141138df975f1b15d2c8e08d4d203c1))
ROM_END

//   YEAR  NAME    PARENT  COMPAT  MACHINE  INPUT    CLASS          INIT        COMPANY                      FULLNAME   FLAGS
COMP(1989, ncd16,  0,      0,      ncd_16,  ncd_68k, ncd_68k_state, empty_init, "Network Computing Devices", "NCD 16",  MACHINE_NOT_WORKING|MACHINE_NO_SOUND)
COMP(1990, ncd17c, 0,      0,      ncd_17c, ncd_68k, ncd_68k_state, empty_init, "Network Computing Devices", "NCD 17C", MACHINE_NOT_WORKING|MACHINE_NO_SOUND)
COMP(1990, ncd19,  0,      0,      ncd_19,  ncd_68k, ncd_68k_state, empty_init, "Network Computing Devices", "NCD 19",  MACHINE_NOT_WORKING|MACHINE_NO_SOUND)