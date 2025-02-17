#include <tb_x64.h>
#include <tb_coff.h>

int run_objdump(int argc, const char** argv) {
    #ifdef _WIN32
    int _setmode(int, int);
    _setmode(0, 0x8000);
    _setmode(1, 0x8000);
    #endif

    if (argc < 1) {
        fprintf(stderr, "\x1b[31merror\x1b[0m: no input files!\n");
        return EXIT_FAILURE;
    }

    FileMap fm = open_file_map(argv[0]);
    if (fm.data == NULL) {
        fprintf(stderr, "\x1b[31merror\x1b[0m: '%s' not found!\n", argv[0]);
        return EXIT_FAILURE;
    }

    TB_COFF_Parser parser = { { strlen(argv[0]), (const uint8_t*) argv[0] }, { fm.size, fm.data } };
    if (!tb_coff_parse_init(&parser)) {
        // TODO(NeGate): make better errors
        fprintf(stderr, "\x1b[31merror\x1b[0m: '%s' was not a valid COFF object!\n", argv[0]);
        return EXIT_FAILURE;
    }

    TB_ObjectSection* sections = cuik_malloc(parser.section_count * sizeof(TB_ObjectSection));

    printf("SECTIONS:\n");
    printf("Idx Name          Size     Address          Type\n");
    for (size_t i = 0; i < parser.section_count; i++) {
        TB_ObjectSection* restrict s = &sections[i];
        tb_coff_parse_section(&parser, i, s);

        char name[9];
        int len = s->name.length > 8 ? 8 : s->name.length;
        memcpy(name, s->name.data, len);
        name[len] = 0;

        size_t size = s->raw_data.length;
        if (size < s->virtual_size) size = s->virtual_size;

        char flags[7];
        flags[0] = (s->flags & TB_COFF_SECTION_READ)    ? 'r' : '-';
        flags[1] = (s->flags & TB_COFF_SECTION_EXECUTE) ? 'x' : '-';
        flags[2] = (s->flags & TB_COFF_SECTION_WRITE)   ? 'w' : '-';
        flags[3] = (s->flags & TB_COFF_SECTION_CODE)    ? 'c' : '-';
        flags[4] = (s->flags & TB_COFF_SECTION_UNINIT)  ? 'u' : '-';
        flags[5] = (s->flags & TB_COFF_SECTION_INIT)    ? 'i' : '-';
        flags[6] = 0;

        printf("%3zu %-8s      %08zx %016zx %s", i, name, size, s->virtual_address, flags);
        if (s->flags & TB_COFF_SECTION_ALIGN) {
            printf(" (align %d)", 1u << (s->flags >> 20));
        }
        printf("\n");
    }
    printf("\n  note:\n");
    printf("    r - read   w - write   x - execute\n");
    printf("    c - code   u - uninit  i - init\n");

    printf("\nSYMBOLS:\n");
    printf("Index Value    Section  Storage   Name\n");
    for (size_t i = 0; i < parser.symbol_count;) {
        TB_ObjectSymbol s;
        i += tb_coff_parse_symbol(&parser, i, &s);

        printf("%5u %08x ", s.ordinal, s.value);
        if (s.section_num > 0) {
            TB_ObjectSection* sec = &sections[s.section_num - 1];

            char name[9];
            int len = sec->name.length > 8 ? 8 : sec->name.length;
            memcpy(name, sec->name.data, len);
            name[len] = 0;

            printf("%-8s ", name);
        } else {
            printf("*none*   ");
        }

        switch (s.type) {
            case TB_OBJECT_SYMBOL_EXTERN:      printf("EXTERN  "); break;
            case TB_OBJECT_SYMBOL_WEAK_EXTERN: printf("WEAK    "); break;
            case TB_OBJECT_SYMBOL_STATIC:      printf("STATIC  "); break;
            default: printf("???     "); break;
        }
        printf("| %.*s\n", (int) s.name.length, s.name.data);
    }

    printf("\n");

    // print disassembly
    for (size_t i = 0; i < parser.section_count; i++) {
        TB_ObjectSection* s = &sections[i];

        size_t size = s->raw_data.length;
        if (size == 0) {
            printf("~~empty~~\n\n");
            continue;
        }

        const uint8_t* data = s->raw_data.data;
        if (0 && (s->flags & TB_COFF_SECTION_CODE)) {
            // dump assembly
            size_t current = 0;
            while (current < size) {
                TB_X86_Inst inst;
                if (tb_x86_disasm(&inst, size - current, &data[current])) {
                    // print instruction
                    printf("  %02x ", inst.opcode);

                    uint8_t flags = inst.flags;
                    for (int i = 0; i < 4; i++) {
                        int8_t reg = inst.regs[i];
                        if (reg < 0) {
                            if (flags & TB_X86_INSTR_USE_MEMOP) {
                                flags &= ~TB_X86_INSTR_USE_MEMOP;
                                assert(0 && "TODO");
                            } else if (flags & TB_X86_INSTR_IMMEDIATE) {
                                flags &= ~TB_X86_INSTR_IMMEDIATE;
                                assert(0 && "TODO");
                            } else {
                                break;
                            }
                        } else {
                            if (i) printf(", ");
                            printf("%s", tb_x86_reg_name(reg, inst.data_type));
                        }
                    }

                    printf("\n");
                    current += inst.length;
                } else {
                    assert(0 && "TODO");
                }
            }
        } else {
            // dump raw data
            printf("DUMP: %.*s\n  ", (int) s->name.length, s->name.data);

            size_t j = 0;
            for (; j < size; j++) {
                printf("%02x ", data[j]);
                if ((j+1) % 16 == 0) {
                    char tmp[16];
                    for (size_t k = 0; k < 16; k++) {
                        uint8_t ch = data[j - 16 + k];
                        tmp[k] = ch < 32 ? '.' : ch;
                    }

                    printf("  %.*s\n  ", 16, tmp);
                }
            }

            int remaining = size % 16;
            if (remaining > 0 && remaining < 16) {
                printf("%*s", (16 - remaining) * 3, "");

                char tmp[16];
                for (size_t k = 0; k < remaining; k++) {
                    uint8_t ch = data[j - remaining + k];
                    tmp[k] = ch < 32 ? '.' : ch;
                }

                printf("  %.*s\n\n", remaining, tmp);
            } else {
                printf("\n");
            }
        }
    }

    return 0;
}
