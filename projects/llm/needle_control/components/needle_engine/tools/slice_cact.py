#!/usr/bin/env python3
"""Cut a Needle 3 .cact down to an official ladder rung.

`needle build --layers N` is the wrong tool here: it wants JAX and re-exports
the checkpoint at 4-bit. This copies tensors from the published 2-bit archive.
"""
import argparse
import os
import struct
import sys

import numpy as np

# ======Settings=========
TAG = 0x05E12A84
ALIGN = 64
HDR_BYTES = 196
REC_SIZE = 44
DEFAULT_IN = "model/needle3.cact"
DEFAULT_OUT = "model/needle3-l6.cact"
DEFAULT_LAYERS = 6
# =======================


def ladder_order(n):
    selected = [0, n - 1]
    order = list(selected)
    while len(order) < n:
        selected.sort()
        _gap, left, right = max(
            ((right - left, left, right)
             for left, right in zip(selected, selected[1:])
             if right - left > 1),
            key=lambda item: (item[0], -item[1]),
        )
        cand = (left + right) // 2
        selected.append(cand)
        order.append(cand)
    return tuple(order)


def align(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def read_recs(raw, num_tensors, dir_off):
    recs = []
    for i in range(num_tensors):
        o = dir_off + i * REC_SIZE
        dtype, ndim, _pad = struct.unpack_from("<BBH", raw, o)
        shape = list(struct.unpack_from("<IIII", raw, o + 4))
        offset, nbytes, group, bits = struct.unpack_from("<QQII", raw, o + 20)
        recs.append({
            "dtype": dtype, "ndim": ndim, "shape": shape,
            "offset": offset, "nbytes": nbytes, "group": group, "bits": bits,
            "blob": raw[offset:offset + nbytes],
        })
    return recs


def cq_take_rows(rec, rows):
    out, in_dim = rec["shape"][0], rec["shape"][1]
    group, bits = rec["group"], rec["bits"]
    in_pad = (in_dim + group - 1) // group * group
    row_bytes = in_pad * bits // 8
    norm_bytes = (in_pad // group) * 2
    blob = rec["blob"]
    idx = blob[:out * row_bytes]
    norms = blob[out * row_bytes:]
    out_rec = dict(rec)
    out_rec["blob"] = (
        b"".join(idx[r * row_bytes:(r + 1) * row_bytes] for r in rows)
        + b"".join(norms[r * norm_bytes:(r + 1) * norm_bytes] for r in rows)
    )
    out_rec["nbytes"] = len(out_rec["blob"])
    sh = list(rec["shape"])
    sh[0] = len(rows)
    out_rec["shape"] = sh
    return out_rec


def fp_take_axis0(rec, rows):
    rest = 1
    for s in rec["shape"][1:rec["ndim"]]:
        rest *= s if s else 1
    item = 2 if rec["dtype"] == 1 else 4
    rowb = rest * item
    blob = rec["blob"]
    out_rec = dict(rec)
    out_rec["blob"] = b"".join(blob[r * rowb:(r + 1) * rowb] for r in rows)
    out_rec["nbytes"] = len(out_rec["blob"])
    sh = list(rec["shape"])
    sh[0] = len(rows)
    out_rec["shape"] = sh
    return out_rec


def fp_take_axis1(rec, idxs):
    dt = np.float16 if rec["dtype"] == 1 else np.float32
    shp = tuple(rec["shape"][:rec["ndim"]])
    a = np.frombuffer(rec["blob"], dt).reshape(shp)
    b = np.ascontiguousarray(a[:, idxs, ...])
    out_rec = dict(rec)
    out_rec["blob"] = b.tobytes()
    out_rec["nbytes"] = len(out_rec["blob"])
    sh = list(rec["shape"])
    sh[1] = len(idxs)
    out_rec["shape"] = sh
    return out_rec


def slice_archive(raw, depth):
    words = list(struct.unpack_from("<49I", raw, 0))
    if words[0] != TAG:
        raise SystemExit("not a Needle 3 .cact (tag 0x05E12A84)")
    num_tensors = words[1]
    cb_len = words[2]
    num_layers = words[10]
    lanes = words[15]
    taps = words[19]
    num_sites = words[31]
    sites = [words[32 + i] for i in range(num_sites)]
    gmask = words[17] | (words[18] << 32)
    global_layers = [i for i in range(num_layers) if (gmask >> i) & 1]
    if not 2 <= depth < num_layers:
        raise SystemExit(f"--layers must be in [2, {num_layers - 1}]")

    dir_off = HDR_BYTES + cb_len * 4
    codebook = raw[HDR_BYTES:dir_off]
    recs = read_recs(raw, num_tensors, dir_off)
    if recs[-1]["dtype"] != 4:
        raise SystemExit("last tensor is not the RAW tokenizer")

    stride = 27 if taps else 24
    mhc0 = 1 + num_layers * stride
    eg0 = mhc0 + 11
    fnorm = eg0 + num_sites * 4
    heads0 = fnorm + 1

    selected = tuple(sorted(ladder_order(num_layers)[:depth]))
    remap = {layer: i for i, layer in enumerate(selected)}

    kept = [recs[0]]
    for li in selected:
        base = 1 + li * stride
        kept.extend(recs[base:base + stride])
    for j in range(3):
        kept.append(fp_take_axis0(recs[mhc0 + j], selected))
    for j in range(3, 6):
        kept.append(fp_take_axis0(recs[mhc0 + j], selected))
    for j in range(6, 8):
        rows = [li * lanes + r for li in selected for r in range(lanes)]
        kept.append(cq_take_rows(recs[mhc0 + j], rows))
    nn = lanes * lanes
    rows = [li * nn + r for li in selected for r in range(nn)]
    kept.append(cq_take_rows(recs[mhc0 + 8], rows))
    kept.append(recs[mhc0 + 9])
    kept.append(recs[mhc0 + 10])

    new_sites = []
    for si, site in enumerate(sites):
        if site in remap:
            new_sites.append(remap[site])
            base = eg0 + si * 4
            kept.extend(recs[base:base + 4])
    kept.append(recs[fnorm])

    cells = (0,) + tuple(li + 1 for li in selected)
    n_k = recs[heads0 + 1]["shape"][0] // (num_layers + 1)
    kept.append(recs[heads0 + 0])
    probe_rows = [c * n_k + k for c in cells for k in range(n_k)]
    kept.append(cq_take_rows(recs[heads0 + 1], probe_rows))
    kept.append(fp_take_axis0(recs[heads0 + 2], cells))
    kept.append(recs[heads0 + 3])
    kept.append(fp_take_axis1(recs[heads0 + 4], cells))
    kept.append(recs[heads0 + 5])
    kept.append(recs[heads0 + 6])
    kept.append(recs[-1])

    new_g = 0
    for g in global_layers:
        if g in remap:
            new_g |= 1 << remap[g]

    hdr = list(words)
    hdr[1] = len(kept)
    hdr[10] = depth
    hdr[17] = new_g & 0xFFFFFFFF
    hdr[18] = (new_g >> 32) & 0xFFFFFFFF
    hdr[31] = len(new_sites)
    for i in range(16):
        hdr[32 + i] = new_sites[i] if i < len(new_sites) else 0
    header = struct.pack("<48I", *hdr[:48]) + raw[48 * 4:49 * 4]

    pos = len(header) + len(codebook) + len(kept) * REC_SIZE
    blobs = []
    dirbytes = bytearray()
    for t in kept:
        pos = align(pos)
        sh = (list(t["shape"]) + [0, 0, 0, 0])[:4]
        dirbytes += struct.pack(
            "<BBHIIIIQQII",
            t["dtype"], t["ndim"], 0, *sh,
            pos, t["nbytes"], t["group"], t["bits"],
        )
        blobs.append((pos, t["blob"]))
        pos += t["nbytes"]

    out = bytearray(header + codebook + dirbytes)
    for off, blob in blobs:
        if len(out) < off:
            out.extend(b"\x00" * (off - len(out)))
        out.extend(blob)
    return bytes(out), selected, new_sites


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="src", default=DEFAULT_IN)
    ap.add_argument("--out", dest="dst", default=DEFAULT_OUT)
    ap.add_argument("--layers", type=int, default=DEFAULT_LAYERS)
    args = ap.parse_args()
    raw = open(args.src, "rb").read()
    buf, selected, sites = slice_archive(raw, args.layers)
    os.makedirs(os.path.dirname(os.path.abspath(args.dst)) or ".", exist_ok=True)
    open(args.dst, "wb").write(buf)
    print(
        f"wrote {args.dst}  {len(buf) / (1024 * 1024):.2f} MiB  "
        f"layers {args.layers} {selected}  engram_sites {sites}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
