fn main() {
    let mut v = vec![(0, 0), (50, 0), (99, 1)];
    v.dedup_by(|a, b| a.1 == b.1);
    println!("{:?}", v);
}
