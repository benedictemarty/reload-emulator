#!/bin/sh
# TinyUSB 0.21.0 (RP2040 host) : erreur de séquence de données sans panic().
#
# Constaté sur la Neo6502 (2026-10-02) : clavier HID et clé USB derrière le hub, une lecture
# de bloc lancée par l'émulation (Apple IIe, Total Replay) arrêtait le processeur dans
# hcd_rp2040_irq, panic("Data Seq Error"), 1 démarrage sur 4. Problème connu en amont,
# non corrigé (tinyusb #3964, cause et correctif proposés dans #3533). Ce correctif :
#  1. hcd_rp2040.c : DATA_SEQ_ERROR traité en premier (sinon le BUFF_STATUS du même
#     instantané accepte les données au mauvais PID), transfert EPX arrêté et signalé en
#     échec, compteur hcd_rp2040_data_seq_errors ; hcd_edpt_clear_stall remet le PID de
#     l'endpoint à DATA0 (il faisait panic()), pour la reprise Bulk-Only ;
#  2. msc_host.c : un transfert en échec termine la commande SCSI en échec (CSW « phase
#     error ») au lieu d'enchaîner sur un CSW périmé ; tuh_msc_bot_info / tuh_msc_abort pour
#     la reprise faite par platforms/rp2040/src/msc_app.c.
# Appliqué à la copie de travail du sous-module, de façon idempotente.
# $1 = dossier source de TinyUSB.
set -e
D="$1/src"
H="$D/portable/raspberrypi/rp2040/hcd_rp2040.c"
M="$D/class/msc/msc_host.c"
MH="$D/class/msc/msc_host.h"
for f in "$H" "$M" "$MH"; do [ -f "$f" ] || { echo "TinyUSB : $f absent" >&2; exit 1; }; done
if grep -q "NEO6502_DATA_SEQ" "$H" && grep -q "NEO6502_DATA_SEQ" "$M" && grep -q "TUH_MSC_NEO6502_RECOVERY" "$MH"; then
    echo "TinyUSB : correctif Data Seq déjà appliqué"
    exit 0
fi
python3 - "$H" "$M" "$MH" <<'EOF'
import sys
h, m, mh = sys.argv[1:4]

def edit(path, pairs):
    s = open(path).read()
    if "NEO6502_DATA_SEQ" in s:
        return
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit("TinyUSB : motif introuvable ou multiple dans %s : %r" % (path, old[:60]))
        s = s.replace(old, new)
    open(path, "w").write(s)

edit(h, [
("""static void __tusb_irq_path_func(hcd_rp2040_irq)(void) {
  const uint32_t status = usb_hw->ints;
""",
"""// NEO6502_DATA_SEQ (reload-emulator, patches/tinyusb-data-seq.sh) : erreurs de séquence vues
volatile uint32_t hcd_rp2040_data_seq_errors;

static void __tusb_irq_path_func(hcd_rp2040_irq)(void) {
  uint32_t status = usb_hw->ints;

  // NEO6502_DATA_SEQ : data PID out of sequence (tinyusb #3964, #3533) : the EPX transfer is
  // stopped and reported as failed instead of panic(). Handled first, so that the BUFF_STATUS
  // of the same snapshot does not accept the wrong-PID data as a completion
  if (status & USB_INTS_ERROR_DATA_SEQ_BITS) {
    usb_hw_clear->sie_status = USB_SIE_STATUS_DATA_SEQ_ERROR_BITS;
    hcd_rp2040_data_seq_errors++;
    if (epx->state == EPSTATE_ACTIVE) {
      usb_hw->sie_ctrl = (usb_hw->sie_ctrl & SIE_CTRL_BASE_MASK) | USB_SIE_CTRL_STOP_TRANS_BITS;
      usbh_dpram->epx_buf_ctrl = 0;
      usb_hw_clear->buf_status = 1u;   // Drop the EPX completion of this snapshot
      xfer_complete_isr(epx, XFER_RESULT_FAILED, false);
    }
    status &= ~(uint32_t)USB_INTS_TRANS_COMPLETE_BITS;
  }
"""),
("""  if (status & USB_INTS_ERROR_DATA_SEQ_BITS) {
    usb_hw_clear->sie_status = USB_SIE_STATUS_DATA_SEQ_ERROR_BITS;
    panic("Data Seq Error \\n");
  }
}""",
"""}"""),
("""  (void)rhport;
  (void)dev_addr;
  (void)ep_addr;

  panic("hcd_clear_stall");
  // return true;
}""",
"""  (void)rhport;
  // NEO6502_DATA_SEQ : after CLEAR_FEATURE(ENDPOINT_HALT) the device toggle is DATA0 again
  hw_endpoint_t *ep = edpt_find(dev_addr, ep_addr);
  if (ep == NULL) return false;
  ep->next_pid = 0;
  return true;
}"""),
])

edit(m, [
("""  switch (p_msc->stage) {
    case MSC_STAGE_CMD:""",
"""  // NEO6502_DATA_SEQ : a failed transfer ends the command as failed (CSW phase error)
  // instead of going on with a stale CSW; the caller runs the Bulk-Only reset recovery
  if (event != XFER_RESULT_SUCCESS && event != XFER_RESULT_STALLED && p_msc->stage != MSC_STAGE_IDLE) {
    p_msc->stage = MSC_STAGE_IDLE;
    csw->status  = MSC_CSW_STATUS_PHASE_ERROR;
    if (p_msc->complete_cb != NULL) {
      tuh_msc_complete_data_t const cb_data = {
          .cbw = cbw,
          .csw = csw,
          .scsi_data = p_msc->buffer,
          .user_arg = p_msc->complete_arg
      };
      (void) p_msc->complete_cb(dev_addr, &cb_data);
    }
    return true;
  }

  switch (p_msc->stage) {
    case MSC_STAGE_CMD:"""),
("""bool tuh_msc_ready(uint8_t dev_addr) {""",
"""// NEO6502_DATA_SEQ : Bulk-Only reset recovery data (interface, bulk endpoints)
bool tuh_msc_bot_info(uint8_t dev_addr, uint8_t* itf_num, uint8_t* ep_in, uint8_t* ep_out) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  TU_VERIFY(p_msc->mounted);
  *itf_num = p_msc->itf_num;
  *ep_in   = p_msc->ep_in;
  *ep_out  = p_msc->ep_out;
  return true;
}

// NEO6502_DATA_SEQ : abort the command in progress (timeout, failure) : endpoints free again
void tuh_msc_abort(uint8_t dev_addr) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  if (!p_msc->mounted) return;
  tuh_edpt_abort_xfer(dev_addr, p_msc->ep_in);
  tuh_edpt_abort_xfer(dev_addr, p_msc->ep_out);
  p_msc->stage = MSC_STAGE_IDLE;
}

bool tuh_msc_ready(uint8_t dev_addr) {"""),
])

edit(mh, [
("""bool tuh_msc_ready(uint8_t dev_addr);""",
"""bool tuh_msc_ready(uint8_t dev_addr);

// NEO6502_DATA_SEQ (reload-emulator) : Bulk-Only reset recovery support
#define TUH_MSC_NEO6502_RECOVERY 1
bool tuh_msc_bot_info(uint8_t dev_addr, uint8_t* itf_num, uint8_t* ep_in, uint8_t* ep_out);
void tuh_msc_abort(uint8_t dev_addr);"""),
])
EOF
grep -q "NEO6502_DATA_SEQ" "$H" && grep -q "NEO6502_DATA_SEQ" "$M" && grep -q "NEO6502_DATA_SEQ" "$MH" \
    || { echo "TinyUSB : échec du correctif Data Seq" >&2; exit 1; }
echo "TinyUSB : correctif Data Seq appliqué"
