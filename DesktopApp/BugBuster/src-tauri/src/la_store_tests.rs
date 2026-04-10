#[cfg(test)]
mod tests {
    use crate::la_store::LaStore;

    #[test]
    fn test_delete_range_simple() {
        let mut store = LaStore::default();
        store.channels = 1;
        store.transitions = vec![vec![(0, 0), (100, 1), (200, 0)]];
        store.total_samples = 300;

        // Delete [120, 150] (inside the '1' pulse)
        // Transitions before: (0,0), (100,1), (200,0)
        // After delete 31 samples: (0,0), (100,1), (169,0)  [200-31 = 169]
        store.delete_range(120, 150);

        assert_eq!(store.total_samples, 269);
        assert_eq!(store.transitions[0].len(), 3);
        assert_eq!(store.transitions[0][0], (0, 0));
        assert_eq!(store.transitions[0][1], (100, 1));
        assert_eq!(store.transitions[0][2], (169, 0));
    }

    #[test]
    fn test_delete_range_bridge() {
        let mut store = LaStore::default();
        store.channels = 1;
        // Pulse from 100 to 200
        store.transitions = vec![vec![(0, 0), (100, 1), (200, 0)]];
        store.total_samples = 300;

        // Delete [50, 150]
        // val_before (at 49) = 0
        // val_after (at 151) = 1
        // Removed = 101 samples.
        // Transition at 200 becomes 200 - 101 = 99.
        // Signal at 50 must now be 1 (val_after).
        store.delete_range(50, 150);

        // Expected transitions:
        // (0, 0)
        // (50, 1)  <- bridge
        // (99, 0)  <- shifted
        assert_eq!(store.transitions[0].len(), 3);
        assert_eq!(store.transitions[0][0], (0, 0));
        assert_eq!(store.transitions[0][1], (50, 1));
        assert_eq!(store.transitions[0][2], (99, 0));
        assert_eq!(store.total_samples, 199);
    }

    #[test]
    fn test_delete_range_rle_dedup() {
        let mut store = LaStore::default();
        store.channels = 1;
        store.transitions = vec![vec![(0, 0), (100, 1), (200, 0), (300, 1)]];
        store.total_samples = 400;

        // Delete [150, 250]
        // val_before (149) = 1
        // val_after (251) = 0
        // Removed = 101.
        // Transition (300, 1) shifts to (199, 1).
        // Signal at 150 becomes 0 (val_after).
        store.delete_range(150, 250);

        // Transitions: (0,0), (100,1), (150,0), (199,1)
        assert_eq!(store.transitions[0].len(), 4);

        // Now delete [120, 180]
        // val_before (119) = 1
        // val_after (181) = 0
        // This should result in (0,0), (100,1), (120,0) ...
        store.delete_range(120, 180);

        // Check RLE dedup (if it happens)
        let mut store2 = LaStore::default();
        store2.channels = 1;
        store2.transitions = vec![vec![(0, 0), (100, 1), (200, 1)]]; // redundant (200, 1)
        store2.delete_range(50, 60); // Trigger dedup
        assert_eq!(store2.transitions[0].len(), 2); // Should be (0,0), (89,1)
    }

    #[test]
    fn test_delete_range_empty_regression() {
        let mut store = LaStore::default();
        store.channels = 1;
        store.transitions = vec![vec![(0, 1)]]; // value is 1 everywhere
        store.total_samples = 100;

        store.delete_range(0, 50);

        // After deleting [0, 50], the remaining 49 samples should still be 1.
        // If transitions array becomes empty, it implies 0.
        // Let's assert what happens.
        assert_eq!(store.get_value_at(0, 0), 1, "Value at 0 should be 1");
    }

    #[test]
    fn test_append_raw_counts_samples_for_one_channel() {
        let mut store = LaStore::from_raw(&[], 1, 1_000_000);
        store.append_raw(&[0b1010_0101]);
        assert_eq!(store.total_samples, 8);
    }

    #[test]
    fn test_append_raw_counts_samples_across_boundaries_for_two_channels() {
        let mut store = LaStore::from_raw(&[], 2, 1_000_000);
        store.append_raw(&[0b11_10_01_00]);
        store.append_raw(&[0b00_01_10_11]);
        assert_eq!(store.total_samples, 8);
    }

    #[test]
    fn test_append_raw_counts_samples_across_boundaries_for_four_channels() {
        let mut store = LaStore::from_raw(&[], 4, 1_000_000);
        store.append_raw(&[0b0001_1110]);
        store.append_raw(&[0b0101_1010, 0b1111_0000]);
        assert_eq!(store.total_samples, 6);
    }
}
