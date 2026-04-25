/* programs/audiotest/main.c -- HD Audio validator (M26F).
 *
 * What this checks (in order):
 *   1. List every device on the AUDIO bus. M26F reports one record
 *      for the controller (hda0) plus one per enumerated codec
 *      (hda0-codecN). Empty list is normal when QEMU is started
 *      without -device intel-hda; we report INFO and move on.
 *   2. Run the "audio" controller self-test. PASS = controller bound +
 *      CORB/RIRB ring up + at least one codec replied to verbs.
 *      SKIP = no controller (or controller present but no codec).
 *   3. Run the "audio_tone" output-stream self-test. PASS = stream
 *      descriptor + BDL + DAC/PIN verbs all accepted by the chip.
 *      SKIP = no codec / no DAC+PIN pair / no output stream slots.
 *      Audibility is a host-audio backend concern (QEMU -audiodev),
 *      not the kernel's; we deliberately don't fail this on silence.
 *
 * Exit codes:
 *   0  -- both probes either PASSed or cleanly SKIPped (no audio HW
 *         at all is treated as PASS for the M26F validation harness)
 *   1  -- one of the probes returned a negative status (driver bug,
 *         register corruption, codec timeout, ...)
 *
 * The controller PASS path also prints the codec topology (DAC/ADC/
 * PIN/MIX widget counts) so test scripts can grep for it.  */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <tobyos_devtest.h>

static const char *result_tag(int rc) {
    if (rc == 0)              return "PASS";
    if (rc == ABI_DEVT_SKIP)  return "SKIP";
    return "FAIL";
}

int main(void) {
    /* ---- Step 1: AUDIO bus listing ---------------------------- */
    static struct abi_dev_info recs[ABI_DEVT_MAX_DEVICES];
    int n = tobydev_list(recs, ABI_DEVT_MAX_DEVICES, ABI_DEVT_BUS_AUDIO);
    if (n < 0) {
        fprintf(stderr, "FAIL: audiotest list: errno=%d\n", errno);
        return 1;
    }
    if (n == 0) {
        printf("INFO: audiotest: no AUDIO bus devices reported by kernel\n");
    } else {
        printf("INFO: audiotest: %d AUDIO record(s) -- "
               "1 controller + %d codec(s)\n",
               n, n - 1);
        tobydev_print_header(stdout);
        for (int i = 0; i < n; i++) tobydev_print_record(stdout, &recs[i]);
    }

    /* ---- Step 2: controller self-test ------------------------- */
    char ctl_msg[ABI_DEVT_MSG_MAX];
    int  ctl_rc = tobydev_test("audio", ctl_msg, sizeof ctl_msg);
    printf("%s: audiotest controller: %s\n",
           result_tag(ctl_rc),
           ctl_msg[0] ? ctl_msg : "(no message)");

    /* ---- Step 3: tone playback self-test ---------------------- */
    char tone_msg[ABI_DEVT_MSG_MAX];
    int  tone_rc = tobydev_test("audio_tone", tone_msg, sizeof tone_msg);
    printf("%s: audiotest tone: %s\n",
           result_tag(tone_rc),
           tone_msg[0] ? tone_msg : "(no message)");

    /* M26F note: SKIP on tone is not a failure -- it just means the
     * QEMU machine has no codec attached, or the host backend is
     * `none`. The kernel still validated the controller. */
    if (tone_rc == ABI_DEVT_SKIP) {
        printf("INFO: audiotest: tone path skipped -- attach a codec via "
               "`-device hda-output` to exercise stream descriptors\n");
    }

    /* Final aggregate. Negative on either probe -> exit 1. */
    if (ctl_rc < 0)  return 1;
    if (tone_rc < 0) return 1;
    return 0;
}
