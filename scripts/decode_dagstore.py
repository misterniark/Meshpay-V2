#!/usr/bin/env python3
"""Décode une partition `dagstore` et dresse la comptabilité des monnaies.

Outil d'audit hors-device (chantier nettoyage currency legacy, 2026-07-15) :
liste les transactions de la fenêtre DAG persistée, puis vérifie la
conservation monétaire par registre (masse émise = somme des soldes), signale
les frais brûlés, les CLAIM ignorées par la défense comptable, les soldes
négatifs planchés et les comptes orphelins.

Usage :
  1) Dumper la partition (T-Deck = flash CLAIRE ; offset dans la table de
     partitions, `gen_esp32part.py partition-table.bin` pour le retrouver) :
       python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXX \
           read_flash 0x190000 0x40000 dagstore.bin
     Les Waveshare (flash encryption) ne sont PAS dumpables : utiliser le
     T-Deck convergé comme sonde du réseau.
  2) Décoder :
       python3 scripts/decode_dagstore.py dagstore.bin

Format (components/dag_store) : 2 slots A/B en double-buffer, chacun
  [header{magic 'DAGS', version, generation, count, record_size}]
  [count × meshpay_tx_t bruts (struct C, ABI Xtensa)]
  [footer{digest, crc32, magic2}]
On charge le slot valide de génération la plus haute, comme le firmware.
Le record_size du header discrimine l'alignement du u64 timestamp_ms
(224 = aligné 4, 232 = aligné 8 — valeur observée sur ESP32-S3).
"""
import struct
import sys
from collections import defaultdict

DAG_STORE_MAGIC = 0x53474144  # 'D','A','G','S' little-endian

TYPES = {1: "TRANSFER", 2: "MINT", 3: "CLAIM"}

# Offsets dans meshpay_tx_t selon record_size (cf. docstring) :
#   id[32] @0, type(i32) @32, from[16] @36, to[16] @52, amount @68, seq @72,
#   fee @76, currency_id @80, puis timestamp_ms (u64, alignement variable),
#   parents[2][32], parent_count, signature[64].
LAYOUTS = {
    224: {"ts": 84, "parents": 92, "pcount": 156},
    232: {"ts": 88, "parents": 96, "pcount": 160},
}


def parse_slot(data, base):
    """Header d'un slot, ou None si le magic ne matche pas."""
    if base + 20 > len(data):
        return None
    magic, version, generation, count, record_size = struct.unpack_from(
        "<5I", data, base)
    if magic != DAG_STORE_MAGIC:
        return None
    return {"base": base, "version": version, "gen": generation,
            "count": count, "rs": record_size}


def decode_txs(data, slot):
    layout = LAYOUTS.get(slot["rs"])
    if layout is None:
        sys.exit(f"record_size inattendu: {slot['rs']} (layout inconnu)")
    txs = []
    for i in range(slot["count"]):
        off = slot["base"] + 20 + i * slot["rs"]
        raw = data[off:off + slot["rs"]]
        pcount = raw[layout["pcount"]]
        txs.append({
            "id": raw[0:32],
            "type": struct.unpack_from("<i", raw, 32)[0],
            "from": raw[36:52],
            "to": raw[52:68],
            "amount": struct.unpack_from("<I", raw, 68)[0],
            "seq": struct.unpack_from("<I", raw, 72)[0],
            "fee": struct.unpack_from("<I", raw, 76)[0],
            "cur": struct.unpack_from("<I", raw, 80)[0],
            "ts": struct.unpack_from("<Q", raw, layout["ts"])[0],
            "parents": [raw[layout["parents"] + j * 32:
                            layout["parents"] + (j + 1) * 32]
                        for j in range(min(pcount, 2))],
        })
    return txs


def audit_currency(txs, cur):
    """Réplique meshpay_currency_get_balance sur un registre + bilans."""
    sel = [t for t in txs if t["cur"] == cur]
    print(f"\n═══ currency_id={cur} ({cur:#010x}) — {len(sel)} tx ═══")

    minters = {t["from"] for t in sel if t["type"] == 2}
    authority = next(iter(minters), None)  # heuristique : le 1er émetteur MINT
    claims = [t for t in sel if t["type"] == 3]
    # initial_credit présumé = montant de CLAIM majoritaire (le firmware le lit
    # dans le descripteur ; hors-device on l'infère).
    init_credit = None
    if claims:
        counts = defaultdict(int)
        for t in claims:
            counts[t["amount"]] += 1
        init_credit = max(counts, key=counts.get)
    print(f"autorités MINT observées: "
          f"{[m.hex()[:8] for m in minters] or '∅'} ; "
          f"CLAIM: {len(claims)} ; crédit initial présumé: {init_credit}")

    bal = defaultdict(int)
    minted = claimed = burned_fees = 0
    for t in sel:
        if t["type"] == 2:  # MINT
            bal[t["to"]] += t["amount"]
            minted += t["amount"]
        elif t["type"] == 3:  # CLAIM
            if t["from"] == t["to"] and t["amount"] == init_credit:
                bal[t["to"]] += t["amount"]
                claimed += t["amount"]
            else:
                print(f"  ⚠ CLAIM ignorée (défense comptable): "
                      f"from={t['from'].hex()[:8]} amount={t['amount']}")
        elif t["type"] == 1:  # TRANSFER
            bal[t["to"]] += t["amount"]
            bal[t["from"]] -= t["amount"] + t["fee"]
            if authority is not None and t["fee"] > 0:
                bal[authority] += t["fee"]
            elif t["fee"] > 0:
                burned_fees += t["fee"]

    has_claim = {t["from"] for t in claims}
    print(f"masse émise = MINT {minted} + CLAIM {claimed} = {minted + claimed}")
    print(f"{'compte':10} {'solde brut':>10} {'affiché(≥0)':>11}")
    for acct, v in sorted(bal.items(), key=lambda kv: -kv[1]):
        tags = []
        if acct == authority:
            tags.append("autorité")
        if claims and acct not in has_claim and acct != authority:
            tags.append("SANS CLAIM (orphelin?)")
        print(f"{acct.hex()[:8]:10} {v:>10} {max(0, v):>11}"
              f"{('  ← ' + ', '.join(tags)) if tags else ''}")
    total_raw = sum(bal.values())
    total_floor = sum(max(0, v) for v in bal.values())
    print(f"{'TOTAL':10} {total_raw:>10} {total_floor:>11}")
    if burned_fees:
        print(f"⚠ frais brûlés (aucune autorité connue): {burned_fees}")
    if total_raw != minted + claimed:
        print(f"⚠ NON-CONSERVATION: total brut {total_raw} ≠ "
              f"masse {minted + claimed}")
    if total_floor != total_raw:
        print(f"⚠ soldes négatifs planchés: l'affichage gonfle de "
              f"{total_floor - total_raw}")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    half = len(data) // 2
    slots = [s for s in (parse_slot(data, 0), parse_slot(data, half)) if s]
    if not slots:
        sys.exit("aucun slot dagstore valide (partition vierge ? chiffrée ?)")
    slot = max(slots, key=lambda s: s["gen"])
    print(f"slot base=0x{slot['base']:x} gen={slot['gen']} "
          f"count={slot['count']} record_size={slot['rs']}")

    txs = decode_txs(data, slot)
    by_id = {t["id"]: t for t in txs}
    print(f"\n{'#':>3} {'type':8} {'from':8} {'to':8} "
          f"{'amount':>6} {'fee':>3} {'seq':>4} {'currency':>10}")
    for i, t in enumerate(txs):
        dangling = sum(1 for p in t["parents"] if p not in by_id)
        note = f"  ({dangling} parent(s) hors fenêtre)" if dangling else ""
        print(f"{i:>3} {TYPES.get(t['type'], str(t['type'])):8} "
              f"{t['from'].hex()[:8]:8} {t['to'].hex()[:8]:8} "
              f"{t['amount']:>6} {t['fee']:>3} {t['seq']:>4} "
              f"{t['cur']:>10}{note}")

    for cur in sorted({t["cur"] for t in txs}):
        audit_currency(txs, cur)


if __name__ == "__main__":
    main()
