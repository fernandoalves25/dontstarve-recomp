/* Regenerate the Vorbis setup headers FMOD's FSB5 encoder references by CRC32.
 * FSB5 Vorbis samples drop the three Vorbis headers and keep only the CRC32
 * of the setup header; the headers come from libvorbis's own VBR encoder, so
 * encoding nothing at every (rate, channels, quality) and hashing the setup
 * packet rebuilds them. Usage: vorbis_setups <crc-hex>... > table
 * Output lines: crc rate channels quality setup-bytes-hex */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vorbis/vorbisenc.h>
#include <zlib.h>
int main(int argc, char **argv) {
    static const int rates[] = { 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000 };
    unsigned long want[64]; int nw = 0, found = 0;
    for (int i = 1; i < argc && nw < 64; i++) want[nw++] = strtoul(argv[i], 0, 16);
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++)
    for (int ch = 1; ch <= 2; ch++)
    for (int q = -10; q <= 100; q++) {
        vorbis_info vi; vorbis_info_init(&vi);
        if (vorbis_encode_init_vbr(&vi, ch, rates[r], q / 100.0f)) { vorbis_info_clear(&vi); continue; }
        vorbis_comment vc; vorbis_comment_init(&vc);
        vorbis_dsp_state vd; vorbis_analysis_init(&vd, &vi);
        ogg_packet id, cm, st;
        vorbis_analysis_headerout(&vd, &vc, &id, &cm, &st);
        unsigned long c = crc32(0, st.packet, (uInt)st.bytes);
        for (int i = 0; i < nw; i++) if (want[i] == c) {
            printf("%08lX %d %d %.2f ", c, rates[r], ch, q / 100.0);
            for (long b = 0; b < st.bytes; b++) printf("%02x", st.packet[b]);
            printf("\n"); want[i] = 0xFFFFFFFFFFFFul; found++;
        }
        vorbis_dsp_clear(&vd); vorbis_comment_clear(&vc); vorbis_info_clear(&vi);
    }
    fprintf(stderr, "matched %d of %d\n", found, nw);
    return 0;
}
