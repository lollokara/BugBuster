fn main() {
    let mut ch_trans = vec![(0, 1), (150, 0)];
    let start = 100;
    let end = 200;
    let removed = end - start + 1;

    let val_before = {
        let idx = ch_trans.partition_point(|&(s, _)| s < start);
        if idx > 0 { Some(ch_trans[idx - 1].1) } else { None }
    };

    ch_trans.retain(|&(s, _)| s < start || s > end);

    for t in ch_trans.iter_mut() {
        if t.0 >= start + removed {
            t.0 -= removed;
        }
    }

    if let Some(vb) = val_before {
        let splice_idx = ch_trans.partition_point(|&(s, _)| s < start);
        let needs_bridge = splice_idx >= ch_trans.len() || ch_trans[splice_idx].1 != vb;
        if needs_bridge && (splice_idx == 0 || ch_trans[splice_idx - 1].1 != vb) {
            ch_trans.insert(splice_idx, (start, vb));
        }
    }

    ch_trans.dedup_by(|a, b| a.1 == b.1);
    println!("{:?}", ch_trans);
}
