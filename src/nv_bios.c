/*
 * Copyright 2005-2006 Erik Waling
 * Copyright 2006 Stephane Marchesin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "nv_include.h"
#include "nvreg.h"
#include <byteswap.h>

/* FIXME: put these somewhere */
#define CRTC_INDEX_COLOR 0x3d4
#define NV_VGA_CRTCX_OWNER_HEADA 0x0
#define NV_VGA_CRTCX_OWNER_HEADB 0x3
#define NV_PBUS_PCI_NV_19	0x0000184C
#define NV_PRAMIN_ROM_OFFSET 0x00700000

#define DEBUGLEVEL 6
/*#define PERFORM_WRITE*/

/* TODO: 
 *       * PLL algorithms.
 */

static int crtchead = 0;

typedef struct {
	Bool execute;
	Bool repeat;
} init_exec_t;

typedef struct {
	uint8_t *data;
	unsigned int length;

	uint16_t init_script_tbls_ptr;
	uint16_t macro_index_tbl_ptr;
	uint16_t macro_tbl_ptr;
	uint16_t condition_tbl_ptr;
	uint16_t io_condition_tbl_ptr;
	uint16_t io_flag_condition_tbl_ptr;
	uint16_t init_function_tbl_ptr;

	uint16_t fptablepointer;
	uint16_t fpxlatetableptr;
	uint16_t lvdsmanufacturerpointer;
	uint16_t fpxlatemanufacturertableptr;
} bios_t;

static uint16_t le16_to_cpu(const uint16_t x)
{
#if X_BYTE_ORDER == X_BIG_ENDIAN
	return bswap_16(x);
#else
	return x;
#endif
}

static uint32_t le32_to_cpu(const uint32_t x)
{
#if X_BYTE_ORDER == X_BIG_ENDIAN
	return bswap_32(x);
#else
	return x;
#endif
}

static Bool nv_cksum(const uint8_t *data, unsigned int length)
{
	/* there's a few checksums in the BIOS, so here's a generic checking function */
	int i;
	uint8_t sum = 0;

	for (i = 0; i < length; i++)
		sum += data[i];

	if (sum)
		return TRUE;

	return FALSE;
}

static int NVValidVBIOS(ScrnInfoPtr pScrn, const uint8_t *data)
{
	/* check for BIOS signature */
	if (!(data[0] == 0x55 && data[1] == 0xAA)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "... BIOS signature not found\n");
		return 0;
	}

	if (nv_cksum(data, data[2] * 512)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "... BIOS checksum invalid\n");
		/* probably ought to set a do_not_execute flag for table parsing here,
		 * assuming most BIOSen are valid */
		return 1;
	} else
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "... appears to be valid\n");

	return 2;
}

static void NVShadowVBIOS_PROM(ScrnInfoPtr pScrn, uint8_t *data)
{
	NVPtr pNv = NVPTR(pScrn);
	int i;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Attempting to locate BIOS image in PROM\n");

	/* enable ROM access */
	nvWriteMC(pNv, 0x1850, 0x0);
	for (i = 0; i < NV_PROM_SIZE; i++) {
		/* according to nvclock, we need that to work around a 6600GT/6800LE bug */
		data[i] = pNv->PROM[i];
		data[i] = pNv->PROM[i];
		data[i] = pNv->PROM[i];
		data[i] = pNv->PROM[i];
		data[i] = pNv->PROM[i];
	}
	/* disable ROM access */
	nvWriteMC(pNv, 0x1850, 0x1);
}

static void NVShadowVBIOS_PRAMIN(ScrnInfoPtr pScrn, uint32_t *data)
{
	NVPtr pNv = NVPTR(pScrn);
	const uint32_t *pramin = (uint32_t *)&pNv->REGS[NV_PRAMIN_ROM_OFFSET/4];
	uint32_t old_bar0_pramin = 0;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Attempting to locate BIOS image in PRAMIN\n");

	if (pNv->Architecture >= NV_ARCH_50) {
		uint32_t vbios_vram;

		vbios_vram = (pNv->REGS[0x619f04/4] & ~0xff) << 8;
		if (!vbios_vram) {
			vbios_vram = pNv->REGS[0x1700/4] << 16;
			vbios_vram += 0xf0000;
		}

		old_bar0_pramin = pNv->REGS[0x1700/4];
		pNv->REGS[0x1700/4] = vbios_vram >> 16;
	}

	memcpy(data, pramin, NV_PROM_SIZE);

	if (pNv->Architecture >= NV_ARCH_50) {
		pNv->REGS[0x1700/4] = old_bar0_pramin;
	}
}

static Bool NVShadowVBIOS(ScrnInfoPtr pScrn, uint32_t *data)
{
	NVShadowVBIOS_PROM(pScrn, (uint8_t *)data);
	if (NVValidVBIOS(pScrn, (uint8_t *)data) == 2)
		return TRUE;

	NVShadowVBIOS_PRAMIN(pScrn, data);
	if (NVValidVBIOS(pScrn, (uint8_t *)data))
		return TRUE;

	return FALSE;
}

typedef struct {
	char* name;
	uint8_t id;
	int length;
	int length_offset;
	int length_multiplier;
	Bool (*handler)(ScrnInfoPtr pScrn, bios_t *, uint16_t, init_exec_t *);
} init_tbl_entry_t;

typedef struct {
	uint8_t id[2];
	uint16_t length;
	uint16_t offset;
} bit_entry_t;

static void parse_init_table(ScrnInfoPtr pScrn, bios_t *bios, unsigned int offset, init_exec_t *iexec);

#define MACRO_INDEX_SIZE        2
#define MACRO_SIZE              8
#define CONDITION_SIZE          12
#define IO_FLAG_CONDITION_SIZE  9 

void still_alive()
{
	sync();
//	usleep(200000);
}

static int nv_valid_reg(uint32_t reg)
{
	#define WITHIN(x,y,z) ((x>=y)&&(x<y+z))
	if (WITHIN(reg,NV_PRAMIN_OFFSET,NV_PRAMIN_SIZE))
		return 1;
	if (WITHIN(reg,NV_PCRTC0_OFFSET,NV_PCRTC0_SIZE))
		return 1;
	if (WITHIN(reg,NV_PRAMDAC0_OFFSET,NV_PRAMDAC0_SIZE))
		return 1;
	if (WITHIN(reg,NV_PFB_OFFSET,NV_PFB_SIZE))
		return 1;
	if (WITHIN(reg,NV_PFIFO_OFFSET,NV_PFIFO_SIZE))
		return 1;
	if (WITHIN(reg,NV_PGRAPH_OFFSET,NV_PGRAPH_SIZE))
		return 1;
	if (WITHIN(reg,NV_PEXTDEV_OFFSET,NV_PEXTDEV_SIZE))
		return 1;
	if (WITHIN(reg,NV_PTIMER_OFFSET,NV_PTIMER_SIZE))
		return 1;
	if (WITHIN(reg,NV_PVIDEO_OFFSET,NV_PVIDEO_SIZE))
		return 1;
	if (WITHIN(reg,NV_PMC_OFFSET,NV_PMC_SIZE))
		return 1;
	if (WITHIN(reg,NV_FIFO_OFFSET,NV_FIFO_SIZE))
		return 1;
	if (WITHIN(reg,NV_PCIO0_OFFSET,NV_PCIO0_SIZE))
		return 1;
	if (WITHIN(reg,NV_PDIO0_OFFSET,NV_PDIO0_SIZE))
		return 1;
	if (WITHIN(reg,NV_PVIO_OFFSET,NV_PVIO_SIZE))
		return 1;
	if (WITHIN(reg,NV_PROM_OFFSET,NV_PROM_SIZE))
		return 1;
	if (WITHIN(reg,NV_PRAMIN_ROM_OFFSET,NV_PROM_SIZE))
		return 1;
	#undef WITHIN
	return 0;
}

static void nv32_rd(ScrnInfoPtr pScrn, uint32_t reg, uint32_t *data)
{
	NVPtr pNv = NVPTR(pScrn);

	if (!nv_valid_reg(reg)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "========= unknown reg 0x%08X ==========\n", reg);
		return;
	}
	*data = pNv->REGS[reg/4];
	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "	Read:  Reg: 0x%08X, Data: 0x%08X\n", reg, *data);
}

static int nv32_wr(ScrnInfoPtr pScrn, uint32_t reg, uint32_t data)
{
	if (DEBUGLEVEL >= 8) {
		uint32_t tmp;
		nv32_rd(pScrn, reg, &tmp);
	}
	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "	Write: Reg: 0x%08X, Data: 0x%08X\n", reg, data);
	if (!nv_valid_reg(reg)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "========= unknown reg 0x%08X ==========\n", reg);
		return 0;
	}
#ifdef PERFORM_WRITE
	still_alive();
	NVPtr pNv = NVPTR(pScrn);
	pNv->REGS[reg/4] = data;
#endif
	return 1;
}

static void nv_port_rd(ScrnInfoPtr pScrn, uint16_t port, uint8_t index, uint8_t *data)
{
	NVPtr pNv = NVPTR(pScrn);
	volatile uint8_t *ptr = crtchead ? pNv->PCIO1 : pNv->PCIO0;

	VGA_WR08(ptr, port, index);
	*data = VGA_RD08(ptr, port + 1);

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "	Indexed read:  Port: 0x%04X, Index: 0x%02X, Head: 0x%02X, Data: 0x%02X\n",
			   port, index, crtchead, *data);
}

static void nv_port_wr(ScrnInfoPtr pScrn, uint16_t port, uint8_t index, uint8_t data)
{
	NVPtr pNv = NVPTR(pScrn);
	volatile uint8_t *ptr;

	if (port == CRTC_INDEX_COLOR && index == NV_VGA_CRTCX_OWNER && data != NV_VGA_CRTCX_OWNER_HEADB)
		crtchead = 0;
	ptr = crtchead ? pNv->PCIO1 : pNv->PCIO0;

	if (DEBUGLEVEL >= 8) {
		uint8_t tmp;
		nv_port_rd(pScrn, port, index, &tmp);
	}
	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "	Indexed write: Port: 0x%04X, Index: 0x%02X, Head: 0x%02X, Data: 0x%02X\n",
			   port, index, crtchead, data);

#ifdef PERFORM_WRITE
	still_alive();
	VGA_WR08(ptr, port, index);
	VGA_WR08(ptr, port + 1, data);
#endif
	if (port == CRTC_INDEX_COLOR && index == NV_VGA_CRTCX_OWNER && data == NV_VGA_CRTCX_OWNER_HEADB)
		crtchead = 1;
}

static Bool io_flag_condition(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, uint8_t cond)
{
	/* The IO flag condition entry has 2 bytes for the CRTC port; 1 byte
	 * for the CRTC index; 1 byte for the mask to apply to the value
	 * retrieved from the CRTC; 1 byte for the shift right to apply to the
	 * masked CRTC value; 2 bytes for the offset to the flag array, to
	 * which the shifted value is added; 1 byte for the mask applied to the
	 * value read from the flag array; and 1 byte for the value to compare
	 * against the masked byte from the flag table.
	 */

	uint16_t condptr = bios->io_flag_condition_tbl_ptr + cond * IO_FLAG_CONDITION_SIZE;
	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[condptr])));
	uint8_t crtcindex = bios->data[condptr + 2];
	uint8_t mask = bios->data[condptr + 3];
	uint8_t shift = bios->data[condptr + 4];
	uint16_t flagarray = le16_to_cpu(*((uint16_t *)(&bios->data[condptr + 5])));
	uint8_t flagarraymask = bios->data[condptr + 7];
	uint8_t cmpval = bios->data[condptr + 8];
	uint8_t data;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X, Shift: 0x%02X, FlagArray: 0x%04X, FAMask: 0x%02X, Cmpval: 0x%02X\n",
			   offset, crtcport, crtcindex, mask, shift, flagarray, flagarraymask, cmpval);

	nv_port_rd(pScrn, crtcport, crtcindex, &data);

	data = bios->data[flagarray + ((data & mask) >> shift)];
	data &= flagarraymask;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Checking if 0x%02X equals 0x%02X\n",
			   offset, data, cmpval);

	if (data == cmpval)
		return TRUE;

	return FALSE;
}

static Bool init_prog(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* INIT_PROG   opcode: 0x31
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): reg
	 * offset + 5  (32 bit): and mask
	 * offset + 9  (8  bit): shift right
	 * offset + 10 (8  bit): number of configurations
	 * offset + 11 (32 bit): register
	 * offset + 15 (32 bit): configuration 1
	 * ...
	 * 
	 * Starting at offset + 15 there are "number of configurations"
	 * 32 bit values. To find out which configuration value to use
	 * read "CRTC reg" on the CRTC controller with index "CRTC index"
	 * and bitwise AND this value with "and mask" and then bit shift the
	 * result "shift right" bits to the right.
	 * Assign "register" with appropriate configuration value.
	 */

	CARD32 reg = *((CARD32 *) (&bios->data[offset + 1]));
	CARD32 and = *((CARD32 *) (&bios->data[offset + 5]));
	CARD8 shiftr = *((CARD8 *) (&bios->data[offset + 9]));
	CARD8 nr = *((CARD8 *) (&bios->data[offset + 10]));
	CARD32 reg2 = *((CARD32 *) (&bios->data[offset + 11]));
	CARD8 configuration;
	CARD32 configval, tmp;

	if (iexec->execute) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: REG: 0x%04X\n", offset, 
				reg);

		nv32_rd(pScrn, reg, &tmp);
		configuration = (tmp & and) >> shiftr;

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CONFIGURATION TO USE: 0x%02X\n", 
				offset, configuration);

		if (configuration <= nr) {

			configval = 
				*((CARD32 *) (&bios->data[offset + 15 + configuration * 4]));

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: REG: 0x%08X, VALUE: 0x%08X\n", offset, 
					reg2, configval);
			
			nv32_rd(pScrn, reg2, &tmp);
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CURRENT VALUE IS: 0x%08X\n",
				offset, tmp);
			nv32_wr(pScrn, reg2, configval);
		}
	}
	return TRUE;
}

static Bool init_io_restrict_prog(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_IO_RESTRICT_PROG   opcode: 0x32 ('2')
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC port
	 * offset + 3  (8  bit): CRTC index
	 * offset + 4  (8  bit): mask
	 * offset + 5  (8  bit): shift
	 * offset + 6  (8  bit): count
	 * offset + 7  (32 bit): register
	 * offset + 11 (32 bit): configuration 1
	 * ...
	 * 
	 * Starting at offset + 11 there are "count" 32 bit values.
	 * To find out which value to use read index "CRTC index" on "CRTC port",
	 * AND this value with "mask" and then bit shift right "shift" bits.
	 * Read the appropriate value using this index and write to "register"
	 */

	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));
	uint8_t crtcindex = bios->data[offset + 3];
	uint8_t mask = bios->data[offset + 4];
	uint8_t shift = bios->data[offset + 5];
	uint8_t count = bios->data[offset + 6];
	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 7])));
	uint8_t config;
	uint32_t configval;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X, Shift: 0x%02X, Count: 0x%02X, Reg: 0x%08X\n",
			   offset, crtcport, crtcindex, mask, shift, count, reg);

	nv_port_rd(pScrn, crtcport, crtcindex, &config);
	config = (config & mask) >> shift;
	if (config > count) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Config 0x%02X exceeds maximal bound 0x%02X\n",
			   offset, config, count);
		return FALSE;
	}

	configval = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 11 + config * 4])));

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Writing config %02X\n", offset, config);

	nv32_wr(pScrn, reg, configval);

	return TRUE;
}

static Bool init_repeat(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_REPEAT   opcode: 0x33 ('3')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): count
	 *
	 * Execute script following this opcode up to INIT_REPEAT_END
	 * "count" times
	 */

	uint8_t count = bios->data[offset + 1];
	uint8_t i;

	/* no iexec->execute check by design */

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "0x%04X: REPEATING FOLLOWING SEGMENT %d TIMES.\n",
		   offset, count);

	iexec->repeat = TRUE;

	/* count - 1, as the script block will execute once when we leave this
	 * opcode -- this is compatible with bios behaviour as:
	 * a) the block is always executed at least once, even if count == 0
	 * b) the bios interpreter skips to the op following INIT_END_REPEAT,
	 * while we don't
	 */
	for (i = 0; i < count - 1; i++)
		parse_init_table(pScrn, bios, offset + 2, iexec);

	iexec->repeat = FALSE;

	return TRUE;
}

static Bool init_io_restrict_pll(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_IO_RESTRICT_PLL   opcode: 0x34 ('4')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC port
	 * offset + 3  (8  bit): CRTC index
	 * offset + 4  (8  bit): mask
	 * offset + 5  (8  bit): shift
	 * offset + 6  (8  bit): IO flag condition index
	 * offset + 7  (8  bit): count
	 * offset + 8  (32 bit): register
	 * offset + 12 (16 bit): frequency 1
	 * ...
	 *
	 * Starting at offset + 12 there are "count" 16 bit frequencies (10kHz).
	 * Set PLL register "register" to coefficients for frequency n,
	 * selected by reading index "CRTC index" of "CRTC port" ANDed with
	 * "mask" and shifted right by "shift". If "IO flag condition index" > 0,
	 * and condition met, double frequency before setting it.
	 */

	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));
	uint8_t crtcindex = bios->data[offset + 3];
	uint8_t mask = bios->data[offset + 4];
	uint8_t shift = bios->data[offset + 5];
	int8_t io_flag_condition_idx = bios->data[offset + 6];
	uint8_t count = bios->data[offset + 7];
	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 8])));
	uint8_t config;
	uint16_t freq;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X, Shift: 0x%02X, IO Flag Condition: 0x%02X, Count: 0x%02X, Reg: 0x%08X\n",
			   offset, crtcport, crtcindex, mask, shift, io_flag_condition_idx, count, reg);

	nv_port_rd(pScrn, crtcport, crtcindex, &config);
	config = (config & mask) >> shift;
	if (config > count) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Config 0x%02X exceeds maximal bound 0x%02X\n",
			   offset, config, count);
		return FALSE;
	}

	freq = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 12 + config * 2])));

	if (io_flag_condition_idx > 0) {
		if (io_flag_condition(pScrn, bios, offset, io_flag_condition_idx)) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: CONDITION FULFILLED - FREQ DOUBLED\n", offset);
			freq *= 2;
		} else
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: CONDITION IS NOT FULFILLED. FREQ UNCHANGED\n", offset);
	}

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, Config: 0x%02X, Freq: %d0kHz\n",
			   offset, reg, config, freq);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: [ NOT YET IMPLEMENTED ]\n", offset);

#if 0
	switch (reg) {
	case 0x00004004:
		configval = 0x01014E07;
		break;
	case 0x00004024:
		configval = 0x13030E02;
		break;
	}
#endif
	return TRUE;
}

static Bool init_end_repeat(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_END_REPEAT   opcode: 0x36 ('6')
	 *
	 * offset      (8 bit): opcode
	 *
	 * Marks the end of the block for INIT_REPEAT to repeat
	 */

	/* no iexec->execute check by design */

	/* iexec->repeat flag necessary to go past INIT_END_REPEAT opcode when
	 * we're not in repeat mode
	 */
	if (iexec->repeat)
		return FALSE;

	return TRUE;
}

static Bool init_copy(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_COPY   opcode: 0x37 ('7')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (8  bit): shift
	 * offset + 6  (8  bit): srcmask
	 * offset + 7  (16 bit): CRTC port
	 * offset + 9  (8 bit): CRTC index
	 * offset + 10  (8 bit): mask
	 *
	 * Read index "CRTC index" on "CRTC port", AND with "mask", OR with
	 * (REGVAL("register") >> "shift" & "srcmask") and write-back to CRTC port
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint8_t shift = bios->data[offset + 5];
	uint8_t srcmask = bios->data[offset + 6];
	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 7])));
	uint8_t crtcindex = bios->data[offset + 9];
	uint8_t mask = bios->data[offset + 10];
	uint32_t data;
	uint8_t crtcdata;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, Shift: 0x%02X, SrcMask: 0x%02X, Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X\n",
			   offset, reg, shift, srcmask, crtcport, crtcindex, mask);

	nv32_rd(pScrn, reg, &data);

	if (shift < 0x80)
		data >>= shift;
	else
		data <<= (0x100 - shift);

	data &= srcmask;

	nv_port_rd(pScrn, crtcport, crtcindex, &crtcdata);
	crtcdata = (crtcdata & mask) | (uint8_t)data;
	nv_port_wr(pScrn, crtcport, crtcindex, crtcdata);

	return TRUE;
}

static Bool init_not(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_NOT   opcode: 0x38 ('8')
	 *
	 * offset      (8  bit): opcode
	 *
	 * Invert the current execute / no-execute condition (i.e. "else")
	 */
	if (iexec->execute)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ------ SKIPPING FOLLOWING COMMANDS  ------\n", offset);
	else
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ------ EXECUTING FOLLOWING COMMANDS ------\n", offset);

	iexec->execute = !iexec->execute;
	return TRUE;
}

static Bool init_io_flag_condition(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_IO_FLAG_CONDITION   opcode: 0x39 ('9')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): condition number
	 *
	 * Check condition "condition number" in the IO flag condition table.
	 * If condition not met skip subsequent opcodes until condition
	 * is inverted (INIT_NOT), or we hit INIT_RESUME
	 */

	uint8_t cond = bios->data[offset + 1];

	if (!iexec->execute)
		return TRUE;

	if (io_flag_condition(pScrn, bios, offset, cond))
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: CONDITION FULFILLED - CONTINUING TO EXECUTE\n", offset);
	else {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: CONDITION IS NOT FULFILLED.\n", offset);
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ------ SKIPPING FOLLOWING COMMANDS  ------\n", offset);
		iexec->execute = FALSE;
	}

	return TRUE;
}

Bool init_idx_addr_latched(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_INDEX_ADDRESS_LATCHED   opcode: 0x49 ('I')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): control register
	 * offset + 5  (32 bit): data register
	 * offset + 9  (32 bit): mask
	 * offset + 13 (32 bit): data
	 * offset + 17 (8  bit): count
	 * offset + 18 (8  bit): address 1
	 * offset + 19 (8  bit): data 1
	 * ...
	 *
	 * For each of "count" address and data pairs, write "data n" to "data register",
	 * read the current value of "control register", and write it back once ANDed
	 * with "mask", ORed with "data", and ORed with "address n"
	 */

	uint32_t controlreg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t datareg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));
	uint32_t mask = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 9])));
	uint32_t data = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 13])));
	uint8_t count = bios->data[offset + 17];
	uint32_t value;
	int i;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ControlReg: 0x%08X, DataReg: 0x%08X, Mask: 0x%08X, Data: 0x%08X, Count: 0x%02X\n",
			   offset, controlreg, datareg, mask, data, count);

	for (i = 0; i < count; i++) {
		uint8_t instaddress = bios->data[offset + 18 + i * 2];
		uint8_t instdata = bios->data[offset + 19 + i * 2];

		if (DEBUGLEVEL >= 6)
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Address: 0x%02X, Data: 0x%02X\n", offset, instaddress, instdata);

		nv32_wr(pScrn, datareg, instdata);

		nv32_rd(pScrn, controlreg, &value);
		value = (value & mask) | data | instaddress;

		nv32_wr(pScrn, controlreg, value);
	}

	return TRUE;
}

static Bool init_io_restrict_pll2(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_IO_RESTRICT_PLL2   opcode: 0x4A ('J')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC port
	 * offset + 3  (8  bit): CRTC index
	 * offset + 4  (8  bit): mask
	 * offset + 5  (8  bit): shift
	 * offset + 6  (8  bit): count
	 * offset + 7  (32 bit): register
	 * offset + 11 (32 bit): frequency 1
	 * ...
	 *
	 * Starting at offset + 11 there are "count" 32 bit frequencies (kHz).
	 * Set PLL register "register" to coefficients for frequency n,
	 * selected by reading index "CRTC index" of "CRTC port" ANDed with
	 * "mask" and shifted right by "shift".
	 */

	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));
	uint8_t crtcindex = bios->data[offset + 3];
	uint8_t mask = bios->data[offset + 4];
	uint8_t shift = bios->data[offset + 5];
	uint8_t count = bios->data[offset + 6];
	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 7])));
	uint8_t config;
	uint32_t freq;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X, Shift: 0x%02X, Count: 0x%02X, Reg: 0x%08X\n",
			   offset, crtcport, crtcindex, mask, shift, count, reg);

	if (!reg)
		return TRUE;

	nv_port_rd(pScrn, crtcport, crtcindex, &config);
	config = (config & mask) >> shift;
	if (config > count) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Config 0x%02X exceeds maximal bound 0x%02X\n",
			   offset, config, count);
		return FALSE;
	}

	freq = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 11 + config * 4])));

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, Config: 0x%02X, Freq: %dkHz\n",
			   offset, reg, config, freq);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: [ NOT YET IMPLEMENTED ]\n", offset);

	return TRUE;
}

static Bool init_pll2(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_PLL2   opcode: 0x4B ('K')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (32 bit): freq
	 *
	 * Set PLL register "register" to coefficients for frequency "freq"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t freq = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%04X, Freq: %dkHz\n",
			   offset, reg, freq);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: [ NOT YET IMPLEMENTED ]\n", offset);

	return TRUE;
}

Bool init_50(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_50   opcode: 0x50 ('P')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): magic lookup value
	 * offset + 2  (8 bit): count
	 * offset + 3  (8 bit): addr 1
	 * offset + 4  (8 bit): data 1
	 * ...
	 *
	 * For each of "count" TMDS address and data pairs write "data n" to "addr n"
	 * "magic lookup value" (mlv) determines which TMDS base address is used:
	 * For mlv < 80, it is an index into a table of TMDS base addresses
	 * For mlv == 80 use the "or" value of the dcb_entry indexed by CR58 for CR57 = 0
	 * to index a table of offsets to the basic 0x6808b0 address
	 * For mlv == 81 use the "or" value of the dcb_entry indexed by CR58 for CR57 = 0
	 * to index a table of offsets to the basic 0x6808b0 address, and then flip the offset by 8
	 */
	NVPtr pNv = NVPTR(pScrn);
	uint8_t mlv = bios->data[offset + 1];
	uint8_t count = bios->data[offset + 2];
	uint32_t reg;
	int i;

	int pramdac_offset[13] = {0, 0, 0x8, 0, 0x2000, 0, 0, 0, 0x2008, 0, 0, 0, 0x2000};
	uint32_t pramdac_table[4] = {0x6808b0, 0x6808b8, 0x6828b0, 0x6828b8};

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: MagicLookupValue: 0x%02X, Count: 0x%02X\n",
			   offset, mlv, count);
	if (mlv >= 0x80) {
		/* here we assume that the DCB table has already been parsed */
		uint8_t dcb_entry;
		int dacoffset;
		nv_port_wr(pScrn, CRTC_INDEX_COLOR, 0x57, 0);
		nv_port_rd(pScrn, CRTC_INDEX_COLOR, 0x58, &dcb_entry);
		if (dcb_entry > pNv->dcb_table.entries) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: CR58 doesn't have a valid DCB entry currently (%02X)\n",
				   offset, dcb_entry);
			return FALSE;
		}
		dacoffset = pramdac_offset[pNv->dcb_table.entry[dcb_entry].or];
		if (mlv == 81)
			dacoffset ^= 8;
		reg = 0x6808b0 + dacoffset;
	} else
		reg = pramdac_table[mlv];

	for (i = 0; i < count; i++) {
		uint8_t tmds_addr = bios->data[offset + 3 + i * 2];
		uint8_t tmds_data = bios->data[offset + 4 + i * 2];

		nv32_wr(pScrn, reg + 4, tmds_data);
		nv32_wr(pScrn, reg, tmds_addr);
	}

	return TRUE;
}
	
Bool init_cr_idx_adr_latch(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_CR_INDEX_ADDRESS_LATCHED   opcode: 0x51 ('Q')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): CRTC index1
	 * offset + 2  (8 bit): CRTC index2
	 * offset + 3  (8 bit): baseaddr
	 * offset + 4  (8 bit): count
	 * offset + 5  (8 bit): data 1
	 * ...
	 *
	 * For each of "count" address and data pairs, write "baseaddr + n" to
	 * "CRTC index1" and "data n" to "CRTC index2"
	 * Once complete, restore initial value read from "CRTC index1"
	 */
	uint8_t crtcindex1 = bios->data[offset + 1];
	uint8_t crtcindex2 = bios->data[offset + 2];
	uint8_t baseaddr = bios->data[offset + 3];
	uint8_t count = bios->data[offset + 4];
	uint8_t oldaddr, data;
	int i;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Index1: 0x%02X, Index2: 0x%02X, BaseAddr: 0x%02X, Count: 0x%02X\n",
			   offset, crtcindex1, crtcindex2, baseaddr, count);

	nv_port_rd(pScrn, CRTC_INDEX_COLOR, crtcindex1, &oldaddr);

	for (i = 0; i < count; i++) {
		nv_port_wr(pScrn, CRTC_INDEX_COLOR, crtcindex1, baseaddr + i);

		data = bios->data[offset + 5 + i];
		nv_port_wr(pScrn, CRTC_INDEX_COLOR, crtcindex2, data);
	}

	nv_port_wr(pScrn, CRTC_INDEX_COLOR, crtcindex1, oldaddr);

	return TRUE;
}

Bool init_cr(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_CR   opcode: 0x52 ('R')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (8  bit): CRTC index
	 * offset + 2  (8  bit): mask
	 * offset + 3  (8  bit): data
	 *
	 * Assign the value of at "CRTC index" ANDed with mask and ORed with data
	 * back to "CRTC index"
	 */

	uint8_t crtcindex = bios->data[offset + 1];
	uint8_t mask = bios->data[offset + 2];
	uint8_t data = bios->data[offset + 3];
	uint8_t value;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Index: 0x%02X, Mask: 0x%02X, Data: 0x%02X\n",
			   offset, crtcindex, mask, data);

	nv_port_rd(pScrn, CRTC_INDEX_COLOR, crtcindex, &value);

	value = (value & mask) | data;

	nv_port_wr(pScrn, CRTC_INDEX_COLOR, crtcindex, value);

	return TRUE;
}

static Bool init_zm_cr(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_CR   opcode: 0x53 ('S')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): CRTC index
	 * offset + 2  (8 bit): value
	 *
	 * Assign "value" to CRTC register with index "CRTC index".
	 */

	uint8_t crtcindex = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint8_t data = bios->data[offset + 2];

	if (!iexec->execute)
		return TRUE;

	nv_port_wr(pScrn, CRTC_INDEX_COLOR, crtcindex, data);

	return TRUE;
}

static Bool init_zm_cr_group(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_CR   opcode: 0x54 ('T')
	 * 
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): count
	 * offset + 2  (8 bit): CRTC index 1
	 * offset + 3  (8 bit): value 1
	 * ...
	 * 
	 * For "count", assign "value n" to CRTC register with index "CRTC index n".
	 */
    
	uint8_t count = bios->data[offset + 1];
	int i;
	
	if (!iexec->execute)
		return TRUE;

	for (i = 0; i < count; i++)
		init_zm_cr(pScrn, bios, offset + 2 + 2 * i - 1, iexec);

	return TRUE;
}

static Bool init_condition_time(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* My BIOS does not use this command. */
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: [ NOT YET IMPLEMENTED ]\n", offset);

	return FALSE;
}

static Bool init_zm_reg_sequence(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_REG_SEQUENCE   opcode: 0x58 ('X')
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): base register
	 * offset + 5  (8  bit): count
	 * offset + 6  (32 bit): value 1
	 * ...
	 * 
	 * Starting at offset + 6 there are "count" 32 bit values.
	 * For "count" iterations set "base register" + 4 * current_iteration
	 * to "value current_iteration"
	 */

	uint32_t basereg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t count = bios->data[offset + 5];
	int i;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: BaseReg: 0x%08X, Count: 0x%02X\n",
			   offset, basereg, count);

	for (i = 0; i < count; i++) {
		uint32_t reg = basereg + i * 4;

		if ((reg & 0xffc) == 0x3c0)
			ErrorF("special case: FIXME\n");
		if ((reg & 0xffc) == 0x3cc)
			ErrorF("special case: FIXME\n");

		uint32_t data = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 6 + i * 4])));

		nv32_wr(pScrn, reg, data);
	}

	return TRUE;
}

static Bool init_indirect_reg(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* INIT_INDIRECT_REG opcode: 0x5A
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (16 bit): adress offset (in bios)
	 *
	 * Lookup value at offset data in the bios and write it to reg
	 */
	NVPtr pNv = NVPTR(pScrn);
	CARD32 reg = *((CARD32 *) (&bios->data[offset + 1]));
	CARD16 data = le16_to_cpu(*((CARD16 *) (&bios->data[offset + 5])));
	CARD32 data2 = bios->data[data];

	if (iexec->execute) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
				"0x%04X: REG: 0x%04X, DATA AT: 0x%04X, VALUE IS: 0x%08X\n", 
				offset, reg, data, data2);

		if (DEBUGLEVEL >= 6) {
			CARD32 tmpval;
			nv32_rd(pScrn, reg, &tmpval);
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CURRENT VALUE IS: 0x%08X\n", offset, tmpval);
		}

		nv32_wr(pScrn, reg, data2);
	}
	return TRUE;
}

static Bool init_sub_direct(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_SUB_DIRECT   opcode: 0x5B ('[')
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): subroutine offset (in bios)
	 *
	 * Calls a subroutine that will execute commands until INIT_DONE
	 * is found. 
	 */

	uint16_t sub_offset = le16_to_cpu(*((uint16_t *) (&bios->data[offset + 1])));

	if (!iexec->execute)
		return TRUE;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: EXECUTING SUB-ROUTINE AT 0x%04X\n",
			offset, sub_offset);

	parse_init_table(pScrn, bios, sub_offset, iexec);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: END OF SUB-ROUTINE AT 0x%04X\n",
			offset, sub_offset);

	return TRUE;
}

static Bool init_copy_nv_reg(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{   
 	CARD32 srcreg = *((CARD32 *) (&bios->data[offset + 1]));
	CARD8 shift = *((CARD8 *) (&bios->data[offset + 5]));
	CARD32 and1 = *((CARD32 *) (&bios->data[offset + 6]));
	CARD32 xor = *((CARD32 *) (&bios->data[offset + 10]));
	CARD32 dstreg = *((CARD32 *) (&bios->data[offset + 14]));
	CARD32 and2 = *((CARD32 *) (&bios->data[offset + 18]));
	CARD32 srcdata;
	CARD32 dstdata;
	
	if (iexec->execute) {
		nv32_rd(pScrn, srcreg, &srcdata);
		
		if (shift > 0)
			srcdata >>= shift;
		else
			srcdata <<= shift;

		srcdata = (srcdata & and1) ^ xor;

		nv32_rd(pScrn, dstreg, &dstdata);
		dstdata &= and2;

		dstdata |= srcdata;

		CARD32 tmp;		
		nv32_rd(pScrn, dstreg, &tmp);

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: REG: 0x%08X, VALUE: 0x%08X\n", offset, dstreg, 
				dstdata);

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CURRENT VALUE IS: 0x%08X\n", offset, tmp);

		nv32_wr(pScrn, dstreg, dstdata);
	}
	return TRUE;
}

static Bool init_zm_index_io(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_INDEX_IO   opcode: 0x62 ('b')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC port
	 * offset + 3  (8  bit): CRTC index
	 * offset + 4  (8  bit): data
	 *
	 * Write "data" to index "CRTC index" of "CRTC port"
	 */
	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));
	uint8_t crtcindex = bios->data[offset + 3];
	uint8_t data = bios->data[offset + 4];

	if (!iexec->execute)
		return TRUE;

	nv_port_wr(pScrn, crtcport, crtcindex, data);

	return TRUE;
}

static Bool init_compute_mem(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_COMPUTE_MEM   opcode: 0x63 ('c')
	 *
	 * offset      (8 bit): opcode
	 *
	 * FIXME
	 */

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: [ NOT YET IMPLEMENTED ]\n", offset);
#if 0
	uint16_t ramcfg = le16_to_cpu(*((uint16_t *)(&bios->data[bios->ram_table_offset])));
	uint32_t pfb_debug;
	uint32_t strapinfo;
	uint32_t ramcfg2;

	if (!iexec->execute)
		return TRUE;

	nv32_rd(pScrn, 0x00101000, &strapinfo);
	nv32_rd(pScrn, 0x00100080, &pfb_debug);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "STRAPINFO: 0x%08X\n", strapinfo);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "PFB_DEBUG: 0x%08X\n", pfb_debug);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "RAM CFG: 0x%04X\n", ramcfg);

	pfb_debug &= 0xffffffef;
	strapinfo >>= 2;
	strapinfo &= 0x0000000f;
	ramcfg2 = le16_to_cpu(*((uint16_t *)
			(&bios->data[bios->ram_table_offset + (2 * strapinfo)])));

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "AFTER MANIPULATION\n");
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "STRAPINFO: 0x%08X\n", strapinfo);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "PFB_DEBUG: 0x%08X\n", pfb_debug);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "RAM CFG2: 0x%08X\n", ramcfg2);


	uint32_t reg1;
	uint32_t reg2;

	nv32_rd(pScrn, 0x00100200, &reg1);
	nv32_rd(pScrn, 0x0010020C, &reg2);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x00100200: 0x%08X\n", reg1);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x0010020C: 0x%08X\n", reg2);
#endif

	return TRUE;
}

static Bool init_reset(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_RESET   opcode: 0x65 ('e')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (32 bit): value1
	 * offset + 9  (32 bit): value2
	 *
	 * Assign "value1" to "register", then assign "value2" to "register"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t value1 = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));
	uint32_t value2 = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 9])));
	uint32_t pci_nv_19;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, Value1: 0x%08X, Value2: 0x%08X\n",
			   offset, reg, value1, value2);

	/* it's not clear from my .dmp file, but it seems we should zero out NV_PBUS_PCI_NV_19(0x0000184C) and then restore it */
	nv32_rd(pScrn, NV_PBUS_PCI_NV_19, &pci_nv_19);
#if 0
	nv32_rd(pScrn, PCICFG(PCICFG_ROMSHADOW), &tmpval);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: PCICFG_ROMSHADOW: 0x%02X\n", offset, tmpval);
#endif
	nv32_wr(pScrn, NV_PBUS_PCI_NV_19, 0);
	nv32_wr(pScrn, reg, value1);
	nv32_wr(pScrn, reg, value2);
	nv32_wr(pScrn, NV_PBUS_PCI_NV_19, pci_nv_19);

	/* PCI Config space init needs to be added here. */
	/* if (nv32_rd(pScrn, PCICFG(PCICFG_ROMSHADOW), value1)) */
	/*     nv32_wr(pScrn, PCICFG(PCICFG_ROMSHADOW), value1 & 0xfffffffe) */

	return TRUE;
}

static Bool init_index_io8(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* INIT_INDEX_IO8   opcode: 0x69
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC reg
	 * offset + 3  (8  bit): and mask
	 * offset + 4  (8  bit): or with
	 * 
	 * 
	 */

	NVPtr pNv = NVPTR(pScrn);
	volatile CARD8 *ptr = crtchead ? pNv->PCIO1 : pNv->PCIO0;
	CARD16 reg = le16_to_cpu(*((CARD16 *)(&bios->data[offset + 1])));
	CARD8 and  = *((CARD8 *)(&bios->data[offset + 3]));
	CARD8 or = *((CARD8 *)(&bios->data[offset + 4]));
	CARD8 data;

	if (iexec->execute) {
		data = (VGA_RD08(ptr, reg) & and) | or;

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
				"0x%04X: CRTC REG: 0x%04X, VALUE: 0x%02X\n", 
				offset, reg, data);
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CURRENT VALUE IS: 0x%02X\n", offset, 
				VGA_RD08(ptr, reg));

#ifdef PERFORM_WRITE
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "init_index_io8 crtcreg 0x%X value 0x%X\n",reg,data);
		still_alive();
		VGA_WR08(ptr, reg, data);
#endif
	}
	return TRUE;
}

static Bool init_sub(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_SUB   opcode: 0x6B ('k')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): script number
	 *
	 * Execute script number "script number", as a subroutine
	 */

	uint8_t sub = bios->data[offset + 1];

	if (!iexec->execute)
		return TRUE;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "0x%04X: EXECUTING SUB-SCRIPT %d\n", offset, sub);

	parse_init_table(pScrn, bios,
			 le16_to_cpu(*((CARD16 *)(&bios->data[bios->init_script_tbls_ptr + sub * 2]))),
			 iexec);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "0x%04X: END OF SUB-SCRIPT %d\n", offset, sub);

	return TRUE;
}

static Bool init_ram_condition(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* INIT_RAM_CONDITION   opcode: 0x6D
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (8  bit): and mask
	 * offset + 2  (8  bit): cmpval
	 *
	 * Test if (NV_PFB_BOOT & and mask) matches cmpval
	 */
	NVPtr pNv = NVPTR(pScrn);
	CARD8 and = *((CARD8 *) (&bios->data[offset + 1]));
	CARD8 cmpval = *((CARD8 *) (&bios->data[offset + 2]));
	CARD32 data;

	if (iexec->execute) {
		data=(pNv->PFB[NV_PFB_BOOT/4])&and;

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
				"0x%04X: CHECKING IF REGVAL: 0x%08X equals COND: 0x%08X\n",
				offset, data, cmpval);

		if (data == cmpval) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
					"0x%04X: CONDITION FULFILLED - CONTINUING TO EXECUTE\n",
					offset);
		} else {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CONDITION IS NOT FULFILLED.\n", offset);
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
					"0x%04X: ------ SKIPPING FOLLOWING COMMANDS  ------\n", offset);
			iexec->execute = FALSE;     
		}
	}
	return TRUE;
}

static Bool init_nv_reg(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_NV_REG   opcode: 0x6E ('n')
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (32 bit): mask
	 * offset + 9  (32 bit): data
	 *
	 * Assign ((REGVAL("register") & "mask") | "data") to "register"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t mask = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));
	uint32_t data = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 9])));
	uint32_t value;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%08X, Mask: 0x%08X, Data: 0x%08X\n",
			   offset, reg, mask, data);

	nv32_rd(pScrn, reg, &value);

	value = (value & mask) | data;

	nv32_wr(pScrn, reg, value);

	return TRUE;
}

static Bool init_macro(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_MACRO   opcode: 0x6F ('o')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): macro number
	 *
	 * Look up macro index "macro number" in the macro index table.
	 * The macro index table entry has 1 byte for the index in the macro table,
	 * and 1 byte for the number of times to repeat the macro.
	 * The macro table entry has 4 bytes for the register address and
	 * 4 bytes for the value to write to that register
	 */

	uint8_t macro_index_tbl_idx = bios->data[offset + 1];
	uint16_t tmp = bios->macro_index_tbl_ptr + (macro_index_tbl_idx * MACRO_INDEX_SIZE);
	uint8_t macro_tbl_idx = bios->data[tmp];
	uint8_t count = bios->data[tmp + 1];
	uint32_t reg, data;
	int i;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Macro: 0x%02X, MacroTableIndex: 0x%02X, Count: 0x%02X\n",
			   offset, macro_index_tbl_idx, macro_tbl_idx, count);

	for (i = 0; i < count; i++) {
		uint16_t macroentryptr = bios->macro_tbl_ptr + (macro_tbl_idx + i) * MACRO_SIZE;

		reg = le32_to_cpu(*((uint32_t *)(&bios->data[macroentryptr])));
		data = le32_to_cpu(*((uint32_t *)(&bios->data[macroentryptr + 4])));

		nv32_wr(pScrn, reg, data);
	}

	return TRUE;
}

static Bool init_done(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_DONE   opcode: 0x71 ('q')
	 *
	 * offset      (8  bit): opcode
	 *
	 * End the current script
	 */

	/* mild retval abuse to stop parsing this table */
	return FALSE;
}

static Bool init_resume(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_RESUME   opcode: 0x72 ('r')
	 *
	 * offset      (8  bit): opcode
	 *
	 * End the current execute / no-execute condition
	 */

	if (iexec->execute)
		return TRUE;

	iexec->execute = TRUE;;
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "0x%04X: ---- EXECUTING FOLLOWING COMMANDS ----\n", offset);

	return TRUE;
}

static Bool init_ram_condition2(ScrnInfoPtr pScrn, bios_t *bios, CARD16 offset, init_exec_t *iexec)
{
	/* INIT_RAM_CONDITION2   opcode: 0x73
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (8  bit): and mask
	 * offset + 2  (8  bit): cmpval
	 *
	 * Test if (NV_EXTDEV_BOOT & and mask) matches cmpval
	 */
	NVPtr pNv = NVPTR(pScrn);
	CARD32 and = *((CARD32 *) (&bios->data[offset + 1]));
	CARD32 cmpval = *((CARD32 *) (&bios->data[offset + 5]));
	CARD32 data;

	if (iexec->execute) {
		data=(nvReadEXTDEV(pNv, NV_PEXTDEV_BOOT))&and;
		
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
				"0x%04X: CHECKING IF REGVAL: 0x%08X equals COND: 0x%08X\n",
				offset, data, cmpval);

		if (data == cmpval) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
					"0x%04X: CONDITION FULFILLED - CONTINUING TO EXECUTE\n",
					offset);
		} else {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  "0x%04X: CONDITION IS NOT FULFILLED.\n", offset);
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,  
					"0x%04X: ------ SKIPPING FOLLOWING COMMANDS  ------\n", offset);
			iexec->execute = FALSE;     
		}
	}
	return TRUE;
}

static Bool init_time(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_TIME   opcode: 0x74 ('t')
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): time
	 * 
	 * Sleep for "time" microseconds.
	 */

	uint16_t time = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Sleeping for 0x%04X microseconds.\n", offset, time);

	usleep(time);

	return TRUE;
}

static Bool init_condition(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_CONDITION   opcode: 0x75 ('u')
	 *
	 * offset      (8 bit): opcode
	 * offset + 1  (8 bit): condition number
	 *
	 * Check condition "condition number" in the condition table.
	 * The condition table entry has 4 bytes for the address of the
	 * register to check, 4 bytes for a mask and 4 for a test value.
	 * If condition not met skip subsequent opcodes until condition
	 * is inverted (INIT_NOT), or we hit INIT_RESUME
	 */

	uint8_t cond = bios->data[offset + 1];
	uint16_t condptr = bios->condition_tbl_ptr + cond * CONDITION_SIZE;
	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[condptr])));
	uint32_t mask = le32_to_cpu(*((uint32_t *)(&bios->data[condptr + 4])));
	uint32_t cmpval = le32_to_cpu(*((uint32_t *)(&bios->data[condptr + 8])));
	uint32_t data;

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Cond: 0x%02X, Reg: 0x%08X, Mask: 0x%08X, Cmpval: 0x%08X\n",
			   offset, cond, reg, mask, cmpval);

	nv32_rd(pScrn, reg, &data);
	data &= mask;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Checking if 0x%08X equals 0x%08X\n",
			   offset, data, cmpval);

	if (data == cmpval) {
		if (DEBUGLEVEL >= 6)
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: CONDITION FULFILLED - CONTINUING TO EXECUTE\n", offset);
	} else {
		if (DEBUGLEVEL >= 6)
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: CONDITION IS NOT FULFILLED.\n", offset);
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ------ SKIPPING FOLLOWING COMMANDS  ------\n", offset);
		iexec->execute = FALSE;
	}

	return TRUE;
}

static Bool init_index_io(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_INDEX_IO   opcode: 0x78 ('x')
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (16 bit): CRTC port
	 * offset + 3  (8  bit): CRTC index
	 * offset + 4  (8  bit): mask
	 * offset + 5  (8  bit): data
	 * 
	 * Read value at index "CRTC index" on "CRTC port", AND with "mask", OR with "data", write-back
	 */

	uint16_t crtcport = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 1])));
	uint8_t crtcindex = bios->data[offset + 3];
	uint8_t mask = bios->data[offset + 4];
	uint8_t data = bios->data[offset + 5];
	uint8_t value;
	
	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Port: 0x%04X, Index: 0x%02X, Mask: 0x%02X, Data: 0x%02X\n",
			   offset, crtcport, crtcindex, mask, data);

	nv_port_rd(pScrn, crtcport, crtcindex, &value);
	value = (value & mask) | data;
	nv_port_wr(pScrn, crtcport, crtcindex, value);

	return TRUE;
}

static Bool init_pll(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_PLL   opcode: 0x79 ('y')
	 *
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (16 bit): freq
	 *
	 * Set PLL register "register" to coefficients for frequency (10kHz) "freq"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint16_t freq = le16_to_cpu(*((uint16_t *)(&bios->data[offset + 5])));

	if (!iexec->execute)
		return TRUE;

	if (DEBUGLEVEL >= 6)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Reg: 0x%04X, Freq: %d0kHz\n",
			   offset, reg, freq);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: [ NOT YET IMPLEMENTED ]\n", offset);

#if 0
	switch (reg) {
		case 0x00680508:
		configval = 0x00011F05;
		break;
	}
#endif
	return TRUE;
}

static Bool init_zm_reg(ScrnInfoPtr pScrn, bios_t *bios, uint16_t offset, init_exec_t *iexec)
{
	/* INIT_ZM_REG   opcode: 0x7A ('z')
	 * 
	 * offset      (8  bit): opcode
	 * offset + 1  (32 bit): register
	 * offset + 5  (32 bit): value
	 * 
	 * Assign "value" to "register"
	 */

	uint32_t reg = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 1])));
	uint32_t value = le32_to_cpu(*((uint32_t *)(&bios->data[offset + 5])));

	if (!iexec->execute)
		return TRUE;

	nv32_wr(pScrn, reg, value);

	return TRUE;
}

static init_tbl_entry_t itbl_entry[] = {
	/* command name                       , id  , length  , offset  , mult    , command handler                 */
	{ "INIT_PROG"                         , 0x31, 15      , 10      , 4       , init_prog                       },
	{ "INIT_IO_RESTRICT_PROG"             , 0x32, 11      , 6       , 4       , init_io_restrict_prog           },
	{ "INIT_REPEAT"                       , 0x33, 2       , 0       , 0       , init_repeat                     },
	{ "INIT_IO_RESTRICT_PLL"              , 0x34, 12      , 7       , 2       , init_io_restrict_pll            },
	{ "INIT_END_REPEAT"                   , 0x36, 1       , 0       , 0       , init_end_repeat                 },
	{ "INIT_COPY"                         , 0x37, 11      , 0       , 0       , init_copy                       },
	{ "INIT_NOT"                          , 0x38, 1       , 0       , 0       , init_not                        },
	{ "INIT_IO_FLAG_CONDITION"            , 0x39, 2       , 0       , 0       , init_io_flag_condition          },
	{ "INIT_INDEX_ADDRESS_LATCHED"        , 0x49, 18      , 17      , 2       , init_idx_addr_latched           },
	{ "INIT_IO_RESTRICT_PLL2"             , 0x4A, 11      , 6       , 4       , init_io_restrict_pll2           },
	{ "INIT_PLL2"                         , 0x4B, 9       , 0       , 0       , init_pll2                       },
/*	{ "INIT_I2C_BYTE"                     , 0x4C, x       , x       , x       , init_i2c_byte                   }, */
/*	{ "INIT_ZM_I2C_BYTE"                  , 0x4D, x       , x       , x       , init_zm_i2c_byte                }, */
/*	{ "INIT_ZM_I2C"                       , 0x4E, x       , x       , x       , init_zm_i2c                     }, */
	{ "INIT_50"                           , 0x50, 3       , 2       , 2       , init_50                         },
	{ "INIT_CR_INDEX_ADDRESS_LATCHED"     , 0x51, 5       , 4       , 1       , init_cr_idx_adr_latch           },
	{ "INIT_CR"                           , 0x52, 4       , 0       , 0       , init_cr                         },
	{ "INIT_ZM_CR"                        , 0x53, 3       , 0       , 0       , init_zm_cr                      },
	{ "INIT_ZM_CR_GROUP"                  , 0x54, 2       , 1       , 2       , init_zm_cr_group                },
	{ "INIT_CONDITION_TIME"               , 0x56, 3       , 0       , 0       , init_condition_time             },
	{ "INIT_ZM_REG_SEQUENCE"              , 0x58, 6       , 5       , 4       , init_zm_reg_sequence            },
	{ "INIT_INDIRECT_REG"                 , 0x5A, 7       , 0       , 0       , init_indirect_reg               },
	{ "INIT_SUB_DIRECT"                   , 0x5B, 3       , 0       , 0       , init_sub_direct                 },
	{ "INIT_COPY_NV_REG"                  , 0x5F, 22      , 0       , 0       , init_copy_nv_reg                },
	{ "INIT_ZM_INDEX_IO"                  , 0x62, 5       , 0       , 0       , init_zm_index_io                },
	{ "INIT_COMPUTE_MEM"                  , 0x63, 1       , 0       , 0       , init_compute_mem                },
	{ "INIT_RESET"                        , 0x65, 13      , 0       , 0       , init_reset                      },
/*	{ "INIT_NEXT"                         , 0x66, x       , x       , x       , init_next                       }, */	
/*	{ "INIT_NEXT"                         , 0x67, x       , x       , x       , init_next                       }, */	
/*	{ "INIT_NEXT"                         , 0x68, x       , x       , x       , init_next                       }, */	
	{ "INIT_INDEX_IO8"                    , 0x69, 5       , 0       , 0       , init_index_io8                  },
	{ "INIT_SUB"                          , 0x6B, 2       , 0       , 0       , init_sub                        },
	{ "INIT_RAM_CONDITION"                , 0x6D, 3       , 0       , 0       , init_ram_condition              },
	{ "INIT_NV_REG"                       , 0x6E, 13      , 0       , 0       , init_nv_reg                     },
	{ "INIT_MACRO"                        , 0x6F, 2       , 0       , 0       , init_macro                      },
	{ "INIT_DONE"                         , 0x71, 1       , 0       , 0       , init_done                       },
	{ "INIT_RESUME"                       , 0x72, 1       , 0       , 0       , init_resume                     },
	{ "INIT_RAM_CONDITION2"               , 0x73, 9       , 0       , 0       , init_ram_condition2             },
	{ "INIT_TIME"                         , 0x74, 3       , 0       , 0       , init_time                       },
	{ "INIT_CONDITION"                    , 0x75, 2       , 0       , 0       , init_condition                  },
/*	{ "INIT_IO_CONDITION"                 , 0x76, x       , x       , x       , init_io_condition               }, */
	{ "INIT_INDEX_IO"                     , 0x78, 6       , 0       , 0       , init_index_io                   },
	{ "INIT_PLL"                          , 0x79, 7       , 0       , 0       , init_pll                        },
	{ "INIT_ZM_REG"                       , 0x7A, 9       , 0       , 0       , init_zm_reg                     },
/*	{ "INIT_RAM_RESTRICT_ZM_REG_GROUP"    , 0x8F, x       , x       , x       , init_ram_restrict_zm_reg_group  }, */
/*	{ "INIT_COPY_ZM_REG"                  , 0x90, x       , x       , x       , init_copy_zm_reg                }, */
/*	{ "INIT_ZM_REG_GROUP_ADDRESS_LATCHED" , 0x91, x       , x       , x       , init_zm_reg_group_addr_latched  }, */
/*	{ "INIT_RESERVED"                     , 0x92, x       , x       , x       , init_reserved                   }, */
	{ 0                                   , 0   , 0       , 0       , 0       , 0                               }
};

static unsigned int get_init_table_entry_length(bios_t *bios, unsigned int offset, int i)
{
	/* Calculates the length of a given init table entry. */
	return itbl_entry[i].length + bios->data[offset + itbl_entry[i].length_offset]*itbl_entry[i].length_multiplier;
}

static void parse_init_table(ScrnInfoPtr pScrn, bios_t *bios, unsigned int offset, init_exec_t *iexec)
{
	/* Parses all commands in a init table. */

	/* We start out executing all commands found in the
	 * init table. Some op codes may change the status
	 * of this variable to SKIP, which will cause
	 * the following op codes to perform no operation until
	 * the value is changed back to EXECUTE.
	 */
	unsigned char id;
	int i;

	int count=0;
	/* Loop until INIT_DONE causes us to break out of the loop
	 * (or until offset > bios length just in case... )
	 * (and no more than 10000 iterations just in case... ) */
	while ((offset < bios->length) && (count++ < 10000)) {
		id = bios->data[offset];

		/* Find matching id in itbl_entry */
		for (i = 0; itbl_entry[i].name && (itbl_entry[i].id != id); i++)
			;

		if (itbl_entry[i].name) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: [ (0x%02X) - %s ]\n",
				   offset, itbl_entry[i].id, itbl_entry[i].name);

			/* execute eventual command handler */
			if (itbl_entry[i].handler)
				if (!(*itbl_entry[i].handler)(pScrn, bios, offset, iexec))
					break;
		} else {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Init table command not found: 0x%02X\n", offset, id);
			break;
		}

		/* Add the offset of the current command including all data
		 * of that command. The offset will then be pointing on the
		 * next op code.
		 */
		offset += get_init_table_entry_length(bios, offset, i);
	}
}

void parse_init_tables(ScrnInfoPtr pScrn, bios_t *bios)
{
	/* Loops and calls parse_init_table() for each present table. */

	int i = 0;
	uint16_t table;
	init_exec_t iexec = {TRUE, FALSE};

	while ((table = le16_to_cpu(*((uint16_t *)(&bios->data[bios->init_script_tbls_ptr + i]))))) {

		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: Parsing init table %d\n",
			table, i / 2);

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: ------ EXECUTING FOLLOWING COMMANDS ------\n", table);
		still_alive();
		parse_init_table(pScrn, bios, table, &iexec);
		i += 2;
	}
}

static void parse_fp_tables(ScrnInfoPtr pScrn, bios_t *bios)
{
	NVPtr pNv = NVPTR(pScrn);
	unsigned int fpstrapping;
	uint8_t *fptable, *fpxlatetable;
/*	uint8_t *lvdsmanufacturertable, *fpxlatemanufacturertable;*/
	unsigned int fpindex;/* lvdsmanufacturerindex;*/
	uint8_t fptable_ver, headerlen = 0, recordlen = 44;
	int ofs;
	DisplayModePtr mode;

	fpstrapping = (nvReadEXTDEV(pNv, NV_PEXTDEV_BOOT) >> 16) & 0xf;

	if (bios->fptablepointer == 0x0) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Pointer to flat panel table invalid\n");
		return;
	}

	fptable = &bios->data[bios->fptablepointer];

	fptable_ver = fptable[0];

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Found flat panel mode table revision %d.%d\n",
		   fptable_ver >> 4, fptable_ver & 0xf);

	switch (fptable_ver) {
	/* PINS version 0x5.0x11 BIOSen have version 1 like tables, but no version field,
	 * and miss one of the spread spectrum/PWM bytes.
	 * This could affect early GF2Go parts (not seen any appropriate ROMs though).
	 * Here we assume that a version of 0x05 matches this case (combining with a
	 * PINS version check would be better), as the common case for the panel type
	 * field is 0x0005, and that is in fact what we are reading the first byte of. */
	case 0x05:	/* some NV10, 11, 15, 16 */
		/* note that in this version the lvdsmanufacturertable is not defined */
		ofs = 6;
		recordlen = 42;
		goto v1common;
	case 0x10:	/* some NV15/16, and NV11+ */
		ofs = 7;
v1common:
		if (bios->fpxlatetableptr == 0x0) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "Pointer to flat panel translation table invalid\n");
			return;
		}
		fpxlatetable = &bios->data[bios->fpxlatetableptr];
	/*	not yet used
		lvdsmanufacturertable = &bios->data[bios->lvdsmanufacturerpointer];
		fpxlatemanufacturertable = &bios->data[bios->fpxlatemanufacturertableptr];*/

		fpindex = fpxlatetable[fpstrapping];
	/*	not yet used
		lvdsmanufacturerindex = fpxlatemanufacturertable[fpstrapping]; */

		if (fpindex > 0xf) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "Bad flat panel table index\n");
			return;
		}
		break;
	case 0x20:	/* NV40+ */
		headerlen = fptable[1];
		recordlen = fptable[2];	// check this, or hardcode as 0x20
/*		may be the wrong test, if there's a translation table
		if (fpstrapping > fptable[3]) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "Flat panel strapping number too high\n");
			return;
		}*/
		ofs = 0;
/*		I don't know where the index for the table comes from in v2.0, so bail
		break;*/
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "FP Table revision not currently supported\n");
		return;
	}

	if (!(mode = xcalloc(1, sizeof(DisplayModeRec))))
		return;

	int modeofs = headerlen + recordlen * fpindex + ofs;
	mode->Clock = le16_to_cpu(*(uint16_t *)&fptable[modeofs]) * 10;
	mode->HDisplay = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 2]);
	mode->HSyncStart = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 10] + 1);
	mode->HSyncEnd = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 12] + 1);
	mode->HTotal = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 14] + 1);
	mode->VDisplay = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 16]);
	mode->VSyncStart = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 24] + 1);
	mode->VSyncEnd = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 26] + 1);
	mode->VTotal = le16_to_cpu(*(uint16_t *)&fptable[modeofs + 28] + 1);
	mode->Flags |= (fptable[modeofs + 30] & 0x10) ? V_PHSYNC : V_NHSYNC;
	mode->Flags |= (fptable[modeofs + 30] & 0x1) ? V_PVSYNC : V_NVSYNC;

	/* for version 1.0:
	 * bytes 1-2 are "panel type", including bits on whether Colour/mono, single/dual link, and type (TFT etc.)
	 * bytes 3-6 are bits per colour in RGBX
	 * 11-12 is HDispEnd
	 * 13-14 is HValid Start
	 * 15-16 is HValid End
	 * bytes 38-39 relate to spread spectrum settings
	 * bytes 40-43 are something to do with PWM */

	mode->prev = mode->next = NULL;
	mode->status = MODE_OK;
	mode->type = M_T_DRIVER | M_T_PREFERRED;
	xf86SetModeDefaultName(mode);

//	if (pNv->debug_modes) { this should exist
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Found flat panel mode in BIOS tables:\n");
		xf86PrintModeline(pScrn->scrnIndex, mode);
//	}

	pNv->fp_native_mode = mode;
}

static void parse_t_table(ScrnInfoPtr pScrn, bios_t *bios, uint16_t ttableptr)
{
	uint8_t headerlen = 0;
	uint16_t table;
	init_exec_t iexec = {TRUE, FALSE};

	if (ttableptr == 0x0) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Pointer to T table invalid\n");
		return;
	}

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Found T table revision %d.%d\n",
		   bios->data[ttableptr] >> 4, bios->data[ttableptr] & 0xf);

	headerlen = bios->data[ttableptr + 1];
	table = ttableptr + headerlen;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "0x%04X: Parsing T table\n", table);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "0x%04X: ------ EXECUTING FOLLOWING COMMANDS ------\n", table);
	parse_init_table(pScrn, bios, table, &iexec);
}

static int parse_bit_display_tbl_entry(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry)
{
	uint16_t table;
	/* Parses the flat panel table segment that the bit entry points to.
	 * Starting at bitentry->offset:
	 *
	 * offset + 0  (16 bits): FIXME table pointer
	 * offset + 2  (16 bits): mode table pointer
	 */

	/* If it's not a laptop, you probably don't care about fptables */
	/* FIXME: detect mobile BIOS? */

	NVPtr pNv = NVPTR(pScrn);

	if (!pNv->Mobile)
		return 1;

	if (bitentry->length != 4) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Do not understand BIT display table entry.\n");
		return 0;
	}

	table = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset])));
	bios->fptablepointer = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 2])));

	parse_fp_tables(pScrn, bios);

	return 1;
}

static unsigned int parse_bit_init_tbl_entry(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry)
{
	/* Parses the init table segment that the bit entry points to.
	 * Starting at bitentry->offset: 
	 * 
	 * offset + 0  (16 bits): init script tables pointer
	 * offset + 2  (16 bits): macro index table pointer
	 * offset + 4  (16 bits): macro table pointer
	 * offset + 6  (16 bits): condition table pointer
	 * offset + 8  (16 bits): io condition table pointer
	 * offset + 10 (16 bits): io flag condition table pointer
	 * offset + 12 (16 bits): init function table pointer
	 *
	 * TODO:
	 * * Are 'I' bit entries always of length 0xE?
	 * 
	 */

	if (bitentry->length < 12) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Unable to recognize BIT init table entry.\n");
		return 0;
	}

	bios->init_script_tbls_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset])));
	bios->macro_index_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 2])));
	bios->macro_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 4])));
	bios->condition_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 6])));
	bios->io_condition_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 8])));
	bios->io_flag_condition_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 10])));
	bios->init_function_tbl_ptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 12])));

	parse_init_tables(pScrn, bios);

	return 1;
}

static int parse_bit_t_tbl_entry(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry)
{
	/* Parses the pointer to the T table
	 *
	 * Starting at bitentry->offset:
	 *
	 * offset + 0  (16 bits): T table pointer
	 */

	uint16_t ttable;

	if (bitentry->length != 2) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Do not understand BIT T table entry.\n");
		return 0;
	}

	ttable = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset])));

	parse_t_table(pScrn, bios, ttable);

	return 1;
}

static unsigned int parse_bmp_table_pointers(ScrnInfoPtr pScrn, bios_t *bios, bit_entry_t *bitentry)
{
	/* Parse the pointers for useful tables in the BMP structure, starting at
	 * offset 75 from the ..NV. signature.
	 *
	 * First 7 pointers as for parse_bit_init_tbl_entry
	 *
	 * offset + 30: flat panel timings table pointer
	 * offset + 32: flat panel strapping translation table pointer
	 * offset + 42: LVDS manufacturer panel config table pointer
	 * offset + 44: LVDS manufacturer strapping translation table pointer
	 */

	NVPtr pNv = NVPTR(pScrn);

	if (!parse_bit_init_tbl_entry(pScrn, bios, bitentry))
		return 0;

	/* If it's not a laptop, you probably don't care about fptables */
	/* FIXME: detect mobile BIOS? */
	if (!pNv->Mobile)
		return 1;

	if (bitentry->length > 33) {
		bios->fptablepointer = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 30])));
		bios->fpxlatetableptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 32])));
	}
	if (bitentry->length > 45) {
		bios->lvdsmanufacturerpointer = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 42])));
		bios->fpxlatemanufacturertableptr = le16_to_cpu(*((uint16_t *)(&bios->data[bitentry->offset + 44])));
	}

	parse_fp_tables(pScrn, bios);

	return 1;
}

static void parse_bit_structure(ScrnInfoPtr pScrn, bios_t *bios, unsigned int offset)
{
	bit_entry_t bitentry;
	char done = 0;

	while (!done) {
		bitentry.id[0] = bios->data[offset];
		bitentry.id[1] = bios->data[offset + 1];
		bitentry.length = le16_to_cpu(*((uint16_t *)&bios->data[offset + 2]));
		bitentry.offset = le16_to_cpu(*((uint16_t *)&bios->data[offset + 4]));

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "0x%04X: Found BIT command with id 0x%02X\n",
			   offset, bitentry.id[0]);

		switch (bitentry.id[0]) {
		case 0:
			/* id[0] = 0 and id[1] = 0 ==> end of BIT struture */
			if (bitentry.id[1] == 0)
				done = 1;
			break;
		case 'D':
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Found flat panel display table entry in BIT structure.\n", offset);
			parse_bit_display_tbl_entry(pScrn, bios, &bitentry);
			break;
		case 'I':
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Found init table entry in BIT structure.\n", offset);
			parse_bit_init_tbl_entry(pScrn, bios, &bitentry);
			break;
		case 'T':
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "0x%04X: Found T table entry in BIT structure.\n", offset);
			parse_bit_t_tbl_entry(pScrn, bios, &bitentry);
			break;
			
			
			/* TODO: What kind of information does the other BIT entrys point to?
			 *       'P' entry is probably performance tables, but there are
			 *       quite a few others...
			 */
		}

		offset += sizeof(bit_entry_t);
	}
}

static void parse_pins_structure(ScrnInfoPtr pScrn, bios_t *bios, unsigned int offset)
{
	int pins_version_major=bios->data[offset+5];
	int pins_version_minor=bios->data[offset+6];
	int init1 = bios->data[offset + 18] + (bios->data[offset + 19] * 256);
	int init2 = bios->data[offset + 20] + (bios->data[offset + 21] * 256);
	int init_size = bios->data[offset + 22] + (bios->data[offset + 23] * 256) + 1;
	int ram_tab;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "PINS version %d.%d\n",
		   pins_version_major, pins_version_minor);

	/* checksum */
	if (nv_cksum(bios->data + offset, 8)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "bad PINS checksum\n");
		return;
	}

	switch (pins_version_major) {
		case 2:
			ram_tab = init1-0x0010;
			break;
		case 3:
		case 4:
		case 5:
			ram_tab = bios->data[offset + 24] + (bios->data[offset + 25] * 256);
			break;
		default:
			return;
	}
	
	if ((pins_version_major==5)&&(pins_version_minor>=6)) {
		/* VCO range info */
	}

	if ((pins_version_major==5)&&(pins_version_minor>=16)) {
		bit_entry_t bitentry;

		if (pins_version_minor == 0x10)
			bitentry.length = 12; /* I've not seen this version, so be "long enough" */
		else if (pins_version_minor < 0x14)
			bitentry.length = 34;
		else
			bitentry.length = 48; /* versions after 0x14 are longer,
						 but extra contents unneeded ATM */

		bitentry.offset = offset + 75;
		parse_bmp_table_pointers(pScrn, bios, &bitentry);
	} else {
		/* TODO type1 script */
	}
}

static unsigned int findstr(bios_t* bios, unsigned char *str, int len)
{
	int i;

	for (i = 2; i <= (bios->length - len); i++)
		if (strncmp((char *)&bios->data[i], (char *)str, len) == 0)
			return i;

	return 0;
}

static Bool parse_dcb_entry(uint8_t dcb_version, uint32_t conn, uint32_t conf, struct dcb_entry *entry)
{
	if (dcb_version >= 0x20) {
		entry->type = conn & 0xf;
		entry->i2c_index = (conn >> 4) & 0xf;
		entry->head = (conn >> 8) & 0xf;
		entry->bus = (conn >> 16) & 0xf;
		entry->location = (conn >> 20) & 0xf;
		entry->or = (conn >> 24) & 0xf;
		if ((1 << ffs(entry->or)) * 3 == entry->or)
			entry->duallink = TRUE;
		else
			entry->duallink = FALSE;
	} else if (dcb_version >= 0x14 ) {
		if (conn != 0xf0003f00) {
			ErrorF("Unknown DCB 1.4 entry, please report\n");
			return FALSE;
		}
		/* safe defaults for a crt */
		entry->type = 0;
		entry->i2c_index = 0;
		entry->head = 1;
		entry->bus = 0;
		entry->location = 0;
		entry->or = 1;
		entry->duallink = FALSE;
	} else {
		// 1.2 needs more loving
		return FALSE;
		entry->type = 0;
		entry->i2c_index = 0;
		entry->head = 0;
		entry->bus = 0;
		entry->location = 0;
		entry->or = 0;
		entry->duallink = FALSE;
	}

	return TRUE;
}

static void
read_dcb_i2c_table(ScrnInfoPtr pScrn, bios_t *bios, uint8_t dcb_version, uint16_t i2ctabptr)
{
	NVPtr pNv = NVPTR(pScrn);
	uint8_t *i2ctable;
	uint8_t headerlen = 0;
	int i2c_entries;
	int recordoffset = 0, rdofs = 1, wrofs = 0;
	int i;

	i2c_entries = MAX_NUM_DCB_ENTRIES;
	memset(pNv->dcb_table.i2c_read, 0, sizeof(pNv->dcb_table.i2c_read));
	memset(pNv->dcb_table.i2c_write, 0, sizeof(pNv->dcb_table.i2c_write));

	i2ctable = &bios->data[i2ctabptr];

	if (dcb_version >= 0x30) {
		if (i2ctable[0] != dcb_version) { /* necessary? */
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "DCB I2C table version mismatch (%02X vs %02X)\n",
				   i2ctable[0], dcb_version);
			return;
		}
		headerlen = i2ctable[1];
		i2c_entries = i2ctable[2];
		if (i2ctable[0] >= 0x40) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "G80 DCB I2C table detected, arrgh\n"); /* they're plain weird */
			return;
		}
	}
	/* it's your own fault if you call this function on a DCB 1.1 BIOS */
	if (dcb_version < 0x14) {
		recordoffset = 2;
		rdofs = 0;
		wrofs = 1;
	}

	for (i = 0; i < i2c_entries; i++) {
		if (i2ctable[headerlen + 4 * i + 3] != 0xff) {
			pNv->dcb_table.i2c_read[i] = i2ctable[headerlen + recordoffset + rdofs + 4 * i];
			pNv->dcb_table.i2c_write[i] = i2ctable[headerlen + recordoffset + wrofs + 4 * i];
		}
	}
}

static unsigned int parse_dcb_table(ScrnInfoPtr pScrn, bios_t *bios)
{
	NVPtr pNv = NVPTR(pScrn);
	uint16_t dcbptr, i2ctabptr = 0;
	uint8_t *dcbtable;
	uint8_t dcb_version, headerlen = 0x4, entries = MAX_NUM_DCB_ENTRIES;
	Bool configblock = TRUE;
	int recordlength = 8, confofs = 4;
	int i;

	pNv->dcb_table.entries = 0;

	/* get the offset from 0x36 */
	dcbptr = le16_to_cpu(*(uint16_t *)&bios->data[0x36]);

	if (dcbptr == 0x0) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "No Display Configuration Block pointer found\n");
		return 0;
	}

	dcbtable = &bios->data[dcbptr];

	/* get DCB version */
	dcb_version = dcbtable[0];
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Display Configuration Block version %d.%d found\n",
		   dcb_version >> 4, dcb_version & 0xf);

	if (dcb_version >= 0x20) { /* NV17+ */
		uint32_t sig;

		if (dcb_version >= 0x30) { /* NV40+ */
			headerlen = dcbtable[1];
			entries = dcbtable[2];
			i2ctabptr = le16_to_cpu(*(uint16_t *)&dcbtable[4]);
			sig = le32_to_cpu(*(uint32_t *)&dcbtable[6]);

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "DCB header length %02X, with %02X possible entries\n",
				   headerlen, entries);
		} else {
			/* dcb_block_count = *(dcbtable[1]); */
			i2ctabptr = le16_to_cpu(*(uint16_t *)&dcbtable[2]);
			sig = le32_to_cpu(*(uint32_t *)&dcbtable[4]);
			headerlen = 8;
		}

		if (sig != 0x4edcbdcb) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "Bad Display Configuration Block signature (%08X)\n", sig);
			return 0;
		}
	} else if (dcb_version >= 0x14) { /* some NV15/16, and NV11+ */
		char sig[8];

		memset(sig, 0, 8);
		strncpy(sig, (char *)&dcbtable[-7], 7);
		/* dcb_block_count = *(dcbtable[1]); */
		i2ctabptr = le16_to_cpu(*(uint16_t *)&dcbtable[2]);
		recordlength = 10;
		confofs = 6;

		if (strcmp(sig, "DEV_REC")) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "Bad Display Configuration Block signature (%s)\n", sig);
			return 0;
		}
	} else if (dcb_version >= 0x12) { /* some NV6/10, and NV15+ */
		/* dcb_block_count = *(dcbtable[1]); */
		i2ctabptr = le16_to_cpu(*(uint16_t *)&dcbtable[2]);
		configblock = FALSE;
	} else {	/* NV5+, maybe NV4 */
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Structure of Display Configuration Blocks prior to version 1.2 unknown\n");
		return 0;
	}

	if (entries >= MAX_NUM_DCB_ENTRIES)
		entries = MAX_NUM_DCB_ENTRIES;

	for (i = 0; i < entries; i++) {
		uint32_t connection, config = 0;

		connection = le32_to_cpu(*(uint32_t *)&dcbtable[headerlen + recordlength * i]);
		if (configblock)
			config = le32_to_cpu(*(uint32_t *)&dcbtable[headerlen + confofs + recordlength * i]);

		/* Should we allow discontinuous DCBs? Certainly DCB I2C tables
		 * can be discontinuous */
		if ((connection & 0x0000000f) == 0x0000000f) /* end of records */
			break;

		ErrorF("Raw DCB entry %d: %08x %08x\n", i, connection, config);
		if (!parse_dcb_entry(dcb_version, connection, config, &pNv->dcb_table.entry[i]))
			break;
	}
	pNv->dcb_table.entries = i;

	read_dcb_i2c_table(pScrn, bios, dcb_version, i2ctabptr);

	return pNv->dcb_table.entries;
}

unsigned int NVParseBios(ScrnInfoPtr pScrn)
{
	unsigned int bit_offset;
	bios_t bios;
	bios.data=NULL;
	bios.fptablepointer = 0;
	uint8_t nv_signature[]={0xff,0x7f,'N','V',0x0};
	uint8_t bit_signature[]={'B','I','T'};
	NVPtr pNv;
	pNv = NVPTR(pScrn);

	pNv->dcb_table.entries = 0;
	pNv->fp_native_mode = NULL;

	pNv->VBIOS = xalloc(64 * 1024);
	if (!NVShadowVBIOS(pScrn, pNv->VBIOS)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "No valid BIOS image found.\n");
		xfree(pNv->VBIOS);
		return 0;
	}
	bios.data = (uint8_t *)pNv->VBIOS;
	bios.length = bios.data[2] * 512;
	if (bios.length > NV_PROM_SIZE)
		bios.length = NV_PROM_SIZE;

	/* parse Display Configuration Block (DCB) table */
	if (parse_dcb_table(pScrn, &bios))
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Found %d entries in DCB.\n", pNv->dcb_table.entries);

	/* check for known signatures */
	if ((bit_offset = findstr(&bios, bit_signature, sizeof(bit_signature)))) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "BIT signature found.\n");
		parse_bit_structure(pScrn, &bios, bit_offset + 4);
	} else if ((bit_offset = findstr(&bios, nv_signature, sizeof(nv_signature)))) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "NV signature found.\n");
		parse_pins_structure(pScrn, &bios, bit_offset);
	} else
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "No known script signature found.\n");

	return 1;
}
