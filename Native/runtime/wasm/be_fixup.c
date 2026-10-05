/*
 * Start-up byte swap of the guest's initialised data (big-endian wasm build).
 *
 * tools/wasm_be_cc.py compiles guest code to see big-endian memory, but the target emits
 * global initialisers in its own (little-endian) byte order. Every object file gets a table
 * of the multi-byte values in its initialised globals and a constructor that hands it here.
 * This file is compiled natively (wasm_be_cc.py --native): it reads the tables as they are.
 */
typedef struct BeFixup
{
    unsigned char *addr;  /* first value */
    unsigned int count;   /* values */
    unsigned int layout;  /* size << 16 | stride */
} BeFixup;

void port_be_fixup(const BeFixup *table, unsigned int num)
{
    unsigned int i, k;

    for (i = 0; i < num; i++)
    {
        unsigned int size = table[i].layout >> 16, stride = table[i].layout & 0xFFFF;
        unsigned char *p = table[i].addr;

        for (k = 0; k < table[i].count; k++, p += stride)
        {
            unsigned char t;

            switch (size)
            {
            case 2:
                t = p[0]; p[0] = p[1]; p[1] = t;
                break;
            case 4:
                t = p[0]; p[0] = p[3]; p[3] = t;
                t = p[1]; p[1] = p[2]; p[2] = t;
                break;
            case 8:
                t = p[0]; p[0] = p[7]; p[7] = t;
                t = p[1]; p[1] = p[6]; p[6] = t;
                t = p[2]; p[2] = p[5]; p[5] = t;
                t = p[3]; p[3] = p[4]; p[4] = t;
                break;
            default:
            {
                unsigned int a = 0, b = size - 1;

                for (; a < b; a++, b--)
                {
                    t = p[a]; p[a] = p[b]; p[b] = t;
                }
                break;
            }
            }
        }
    }
}
