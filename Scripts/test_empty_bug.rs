use bugbuster::la_store::LaStore;

fn main() {
    let mut store = LaStore::default();
    store.channels = 1;
    store.transitions = vec![vec![(0, 1)]]; // value is 1 everywhere
    store.total_samples = 100;

    store.delete_range(0, 50);

    println!("Total samples: {}", store.total_samples);
    println!("Transitions: {:?}", store.transitions[0]);
    println!("Value at 0: {}", store.get_value_at(0, 0));
}
