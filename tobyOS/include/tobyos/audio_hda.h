/* audio_hda.h -- Intel HD Audio (HDA) controller driver.
 *
 * M26A (legacy) stubbed out detection only. M26F is the real driver:
 *
 *   * Full PCI probe + BAR0 mapping.
 *   * Controller reset (GCTL.CRST cycle) per HDA spec, with the
 *     521 us codec-wakeup delay.
 *   * CORB / RIRB DMA rings for codec verb traffic (one 4 KiB page,
 *     split between the two rings).
 *   * Codec enumeration: STATESTS scan -> per-codec Vendor ID, Revision,
 *     subordinate node walk, AFG discovery, widget classification
 *     (DAC / ADC / PIN / MIXER / SELECTOR / OTHER).
 *   * Best-effort tone playback through output stream descriptor 0
 *     using a single-entry BDL. Audible only when QEMU is wired with
 *     a real audio backend; the register/verb sequence still runs
 *     and gets validated either way.
 *
 * Tested QEMU topology:
 *   -audiodev none,id=hda      \
 *   -device   intel-hda,id=hda0,audiodev=hda \
 *   -device   hda-output,bus=hda0.0,audiodev=hda
 *
 * Hardware match (PCI):
 *   class = PCI_CLASS_MULTIMEDIA (0x04)
 *   subclass = HDA (0x03) or AudioDevice (0x80)
 *
 * The driver degrades gracefully on a machine without an HDA controller:
 *   audio_hda_present() stays false, all introspection returns 0,
 *   and audio_hda_selftest()/audio_hda_tone_selftest() return SKIP. */

#ifndef TOBYOS_AUDIO_HDA_H
#define TOBYOS_AUDIO_HDA_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Maximum codecs we'll track per controller. The HDA spec allows 15
 * (4-bit codec address). 4 is plenty for QEMU + every commodity laptop
 * I've seen. */
#define AUDIO_HDA_MAX_CODECS 4

/* Per-codec snapshot exposed via audio_hda_codec_at(). The stats are
 * counts, not handles -- M26F doesn't expose the widget tree itself
 * because nothing in userland uses it yet. */
struct audio_hda_codec_info {
    uint8_t  cad;            /* codec address 0..14 */
    uint16_t vendor;         /* high 16 bits of Vendor ID verb (NID 0, param 0) */
    uint16_t device;         /* low  16 bits of Vendor ID verb */
    uint16_t revision;       /* Revision ID verb (NID 0, param 0x02) */
    uint8_t  afg_nid;        /* node id of the Audio Function Group, 0 if none */
    uint8_t  widget_start;   /* first NID inside the AFG */
    uint8_t  widget_count;   /* number of widget NIDs inside the AFG */
    uint8_t  dacs;           /* output converters (audio output widgets) */
    uint8_t  adcs;           /* input  converters */
    uint8_t  pins;           /* pin complex widgets (jacks / speakers) */
    uint8_t  mixers;
    uint8_t  selectors;
    uint8_t  others;         /* power, beep, vol-knob, vendor-defined, ... */
    uint8_t  first_dac_nid;  /* convenience: NID of the first DAC, 0 if none */
    uint8_t  first_pin_nid;  /* convenience: NID of the first PIN, 0 if none */
};

/* Register the HDA PCI driver so pci_bind_drivers() sees it. Idempotent;
 * safe to call once at boot even if no controller is present. */
void audio_hda_register(void);

/* True after a successful PCI probe found and minimally initialised
 * an HDA controller (M26F: full bring-up, CORB/RIRB up, codecs walked). */
bool audio_hda_present(void);

/* Number of codecs we successfully enumerated on the controller, or 0. */
int  audio_hda_codec_count(void);

/* Snapshot the i-th codec into *out. Returns true on success. */
bool audio_hda_codec_at(int i, struct audio_hda_codec_info *out);

/* Fill an abi_dev_info record describing the controller (and one per
 * codec, up to `cap`); returns the number written. Used by
 * devtest_enumerate. */
int  audio_hda_introspect(struct abi_dev_info *out, int cap);

/* Self-test entry called via devtest_run("audio", ...). Returns:
 *   ABI_DEVT_SKIP  if no controller present (clean QEMU default path,
 *                  i.e. `make run-xhci` without -device intel-hda)
 *   0              if the controller passes the M26F bring-up checks
 *                  (CORB/RIRB up + at least one codec answered verbs)
 *   negative       on any unexpected failure
 * `msg` is filled with a one-line diagnostic. */
int  audio_hda_selftest(char *msg, size_t cap);

/* Tone-playback self-test via devtest_run("audio_tone", ...).
 * Programs output stream 0 with a 1-period square-wave at 440 Hz,
 * 48 kHz / 16-bit / stereo, runs the stream for ~250 ms, then stops
 * it. Returns:
 *   ABI_DEVT_SKIP  no controller / no codec / no DAC+PIN pair / no OSS
 *   0              registers + codec verbs accepted (audibility is
 *                  QEMU-audiodev-dependent and NOT validated here)
 *   negative       register read-back mismatch or stream-descriptor
 *                  reset wedged
 * `msg` is filled with what was actually programmed. */
int  audio_hda_tone_selftest(char *msg, size_t cap);

#endif /* TOBYOS_AUDIO_HDA_H */
