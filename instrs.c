#include "global.h"

#define DEFAULT_POOL_SIZE 262144 // 256k instructions

SmartHash* instrs_pool = 0;

static
void delete_instr(UniqueInstr* instr) {
	LPG_ASSERT(instr != 0);

	if (instr->name)
		LPG_FREE(instr->name);

	if (instr->desc) {
		if (instr->desc->name != 0)
			LPG_FREE(instr->desc->name);

		LPG_DATA_FREE(instr->desc, sizeof(InstrDesc));
	}

	LPG_DATA_FREE(instr, sizeof(UniqueInstr));
}

static
HChar* next_line(Int fd) {
    Int s, idx;
    HChar c;
    static HChar buffer[1024];

    idx = 0;
    VG_(memset)(&buffer, 0, sizeof(buffer));

    while (True) {
		LPG_ASSERT(idx >= 0 && idx < ((sizeof(buffer) / sizeof(HChar))-1));
		s = VG_(read)(fd, &c, 1);
		if (s == 0 || c == '\n')
			break;

		// Ignore carriage returns.
		if (c == '\r')
			continue;

		buffer[idx++] = c;
    }

    return idx > 0 ? buffer : 0;
}

static
void read_instr_names(void) {
	Int fd;
	Addr addr;
	HChar* line;
	HChar* name;
	UniqueInstr* instr;

	if (LPG_(clo).instrs_map) {
		fd = VG_(fd_open)(LPG_(clo).instrs_map, VKI_O_RDONLY, 0);
		if (fd < 0)
			tl_assert(0);

		while ((line = next_line(fd))) {
			name = VG_(strchr)(line, ':');
			if (name) {
				*name = 0;
				name++;

				addr = VG_(strtoull16)(line, 0);
				if (addr != 0 && *name != 0) {
					instr = LPG_(get_instr)(addr, 0);
					instr->name = VG_(strdup)("lg.instrs.rin.1", name);
				}
			}
		}

		VG_(close)(fd);
	}
}

void LPG_(init_instrs_pool)() {
	LPG_ASSERT(instrs_pool == 0);

	instrs_pool = LPG_(new_smart_hash)(DEFAULT_POOL_SIZE);

	// set the growth rate to half the size.
	LPG_(smart_hash_set_growth_rate)(instrs_pool, 1.5f);

	// read instruction names.
	read_instr_names();
}

void LPG_(destroy_instrs_pool)() {
	LPG_ASSERT(instrs_pool != 0);

	LPG_(smart_hash_clear)(instrs_pool, (void (*)(void*)) delete_instr);
	LPG_(delete_smart_hash)(instrs_pool);
	instrs_pool = 0;
}

UniqueInstr* LPG_(get_instr)(Addr addr, Int size) {
	UniqueInstr* instr = LPG_(find_instr)(addr);
	if (instr) {
		LPG_ASSERT(instr->addr == addr);
		if (size != 0) {
			if (instr->size == 0) {
				instr->size = size;
			} else {
				LPG_ASSERT(instr->size == size);
			}
		}
	} else {
		instr = (UniqueInstr*) LPG_MALLOC("lg.instrs.gi.1", sizeof(UniqueInstr));
		VG_(memset)(instr, 0, sizeof(UniqueInstr));
		instr->addr = addr;
		instr->size = size;

		LPG_(smart_hash_put)(instrs_pool, instr, (HWord (*)(void*)) LPG_(instr_addr));
	}

	return instr;
}

UniqueInstr* LPG_(find_instr)(Addr addr) {
	return (UniqueInstr*) LPG_(smart_hash_get)(instrs_pool, addr, (HWord (*)(void*)) LPG_(instr_addr));
}

Addr LPG_(instr_addr)(UniqueInstr* instr) {
	LPG_ASSERT(instr != 0);
	return instr->addr;
}

Int LPG_(instr_size)(UniqueInstr* instr) {
	LPG_ASSERT(instr != 0);
	return instr->size;
}

const HChar* LPG_(instr_name)(UniqueInstr* instr) {
	LPG_ASSERT(instr != 0);
	return instr->name;
}

InstrDesc* LPG_(instr_description)(UniqueInstr* instr) {
	LPG_ASSERT(instr != 0);

	if (!instr->desc) {
		Bool found;
		DiEpoch ep;
		UInt tmpline;
		const HChar *tmpfile, *tmpdir;

		ep = VG_(current_DiEpoch)();
		found = VG_(get_filename_linenum)(ep, instr->addr,
					&(tmpfile), &(tmpdir), &(tmpline));

		instr->desc = (InstrDesc*) LPG_MALLOC("lg.instrs.id.1", sizeof(InstrDesc));
		if (found) {
		    /* Build up an absolute pathname, if there is a directory available */
			instr->desc->name = (HChar*) LPG_MALLOC("lg.adesc.na.1",
		    		(VG_(strlen)(tmpdir) + 1 + VG_(strlen)(tmpfile) + 1));
		    VG_(strcpy)(instr->desc->name, tmpdir);
		    if (instr->desc->name[0] != '\0')
		       VG_(strcat)(instr->desc->name, "/");
		    VG_(strcat)(instr->desc->name, tmpfile);

			instr->desc->lineno = tmpline;
		} else {
			instr->desc->name = 0;
			instr->desc->lineno = -1;
		}
	}

	return instr->desc;
}

Bool LPG_(instrs_cmp)(UniqueInstr* i1, UniqueInstr* i2) {
	return i1 && i2 && i1->addr == i2->addr && i1->size == i2->size;
}

void LPG_(print_instr)(UniqueInstr* instr, Bool complete) {
	LPG_ASSERT(instr != 0);

	VG_(printf)("0x%lx [%d]", instr->addr, instr->size);
	if (complete) {
		VG_(printf)(" (");
		LPG_(print_instr_description)(LPG_(instr_description)(instr));
		VG_(printf)(")");
	}
}

void LPG_(fprint_instr)(VgFile* fp, UniqueInstr* instr, Bool complete) {
	LPG_ASSERT(fp != 0);
	LPG_ASSERT(instr != 0);

	VG_(fprintf)(fp, "0x%lx [%d]", instr->addr, instr->size);
	if (complete) {
		VG_(fprintf)(fp, " (");
		LPG_(fprint_instr_description)(fp, LPG_(instr_description)(instr));
		VG_(fprintf)(fp, ")");
	}
}

void LPG_(print_instr_description)(InstrDesc* idesc) {
	LPG_ASSERT(idesc != 0);

	if (idesc->name)
		VG_(printf)("%s:%d", idesc->name, idesc->lineno);
	else
		VG_(printf)("unknown");
}

void LPG_(fprint_instr_description)(VgFile* fp, InstrDesc* idesc) {
	LPG_ASSERT(fp != 0);
	LPG_ASSERT(idesc != 0);

	if (idesc->name)
		VG_(fprintf)(fp, "%s:%d", idesc->name, idesc->lineno);
	else
		VG_(fprintf)(fp, "unknown");
}
