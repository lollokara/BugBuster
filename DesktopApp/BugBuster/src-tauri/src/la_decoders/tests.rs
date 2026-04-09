#[cfg(test)]
mod tests {
    use crate::la_store::LaStore;
    use crate::la_decoders::{uart, i2c, spi};

    #[test]
    fn test_uart_decode() {
        let mut store = LaStore::default();
        store.channels = 1;
        store.transitions = vec![Vec::new()];
        store.sample_rate_hz = 115200 * 10; // 10 samples per bit
        let spb = 10;

        // Data: 'H' (0x48)
        // LSB first: 0, 0, 0, 1, 0, 0, 1, 0
        let data = [0x48, 0x65, 0x6C, 0x6C, 0x6F];
        let mut current_sample = 100;

        // Initial idle
        store.transitions[0].push((0, 1));

        for &byte in &data {
            // Start bit (0)
            store.transitions[0].push((current_sample, 0));
            current_sample += spb;

            // Data bits
            for i in 0..8 {
                let bit = (byte >> i) & 1;
                let last_val = store.transitions[0].last().unwrap().1;
                if bit != last_val {
                    store.transitions[0].push((current_sample, bit));
                }
                current_sample += spb;
            }

            // Stop bit (1)
            let last_val = store.transitions[0].last().unwrap().1;
            if last_val != 1 {
                store.transitions[0].push((current_sample, 1));
            }
            current_sample += spb;
            
            // Gap between bytes
            current_sample += spb * 2;
        }
        store.total_samples = current_sample;

        let cfg = uart::UartConfig {
            tx_channel: 0,
            baud_rate: 115200,
            ..Default::default()
        };

        let annotations = uart::decode(&cfg, &store, 0, store.total_samples);
        assert_eq!(annotations.len(), 5);
        assert_eq!(annotations[0].text, "'H'");
        assert_eq!(annotations[1].text, "'e'");
        assert_eq!(annotations[2].text, "'l'");
        assert_eq!(annotations[3].text, "'l'");
        assert_eq!(annotations[4].text, "'o'");
    }

    #[test]
    fn test_i2c_decode() {
        let mut store = LaStore::default();
        store.channels = 2; // 0: SDA, 1: SCL
        store.sample_rate_hz = 1_000_000;
        store.transitions = vec![Vec::new(), Vec::new()];

        let mut t = 100;
        let step = 10;

        // Initial Idle: SDA=1, SCL=1
        store.transitions[0].push((0, 1));
        store.transitions[1].push((0, 1));

        // START: SDA falls while SCL=1
        t += step;
        store.transitions[0].push((t, 0));
        t += step;

        // Helper to push a bit
        let mut push_bit = |bit: u8, t_ref: &mut u64| {
            // SCL falls
            store.transitions[1].push((*t_ref, 0));
            *t_ref += step / 2;
            // SDA changes
            let last_sda = store.transitions[0].last().unwrap().1;
            if bit != last_sda {
                store.transitions[0].push((*t_ref, bit));
            }
            *t_ref += step / 2;
            // SCL rises
            store.transitions[1].push((*t_ref, 1));
            *t_ref += step;
        };

        // ADDR 0x3C + W(0) => 0x78
        // Bits: 0, 1, 1, 1, 1, 0, 0, 0
        let addr_byte = 0x78u8;
        for i in (0..8).rev() {
            push_bit((addr_byte >> i) & 1, &mut t);
        }
        // ACK (SDA=0)
        push_bit(0, &mut t);

        // DATA 0xAA => 1010 1010
        let data_byte = 0xAAu8;
        for i in (0..8).rev() {
            push_bit((data_byte >> i) & 1, &mut t);
        }
        // ACK (SDA=0)
        push_bit(0, &mut t);

        // STOP: SDA rises while SCL=1
        // SCL is already 1 from last push_bit
        store.transitions[1].push((t, 1)); // Ensure SCL is high (it should be)
        store.transitions[0].push((t + step/2, 1)); // SDA rises
        t += step;

        store.total_samples = t;

        let cfg = i2c::I2cConfig { sda_channel: 0, scl_channel: 1 };
        let annotations = i2c::decode(&cfg, &store, 0, store.total_samples);

        // Expected: S, 0x3C W, ACK, 0xAA, ACK, P
        // Total 6 annotations
        assert_eq!(annotations.len(), 6);
        assert_eq!(annotations[0].text, "S");
        assert_eq!(annotations[1].text, "0x3C W");
        assert_eq!(annotations[2].text, "ACK");
        assert_eq!(annotations[3].text, "0xAA");
        assert_eq!(annotations[4].text, "ACK");
        assert_eq!(annotations[5].text, "P");
    }

    #[test]
    fn test_spi_decode() {
        let mut store = LaStore::default();
        store.channels = 4; // 0:MOSI, 1:MISO, 2:SCLK, 3:CS
        store.sample_rate_hz = 1_000_000;
        store.transitions = vec![Vec::new(), Vec::new(), Vec::new(), Vec::new()];

        let mut t = 100;
        let step = 10;

        // Initial: CS=1, SCLK=0, MOSI=0, MISO=0
        for ch in 0..4 { store.transitions[ch].push((0, 0)); }
        store.transitions[3][0] = (0, 1); // CS=1

        // CS falls
        t += step;
        store.transitions[3].push((t, 0));
        t += step;

        // Mode 0: CPOL=0, CPHA=0. Data sampled on rising edge.
        // MOSI: 0x55 (0101 0101), MISO: 0xAA (1010 1010)
        let mosi_data = 0x55u8;
        let miso_data = 0xAAu8;

        for i in (0..8).rev() {
            let mosi_bit = (mosi_data >> i) & 1;
            let miso_bit = (miso_data >> i) & 1;

            // Data setup (before rising edge)
            if store.transitions[0].last().unwrap().1 != mosi_bit {
                store.transitions[0].push((t, mosi_bit));
            }
            if store.transitions[1].last().unwrap().1 != miso_bit {
                store.transitions[1].push((t, miso_bit));
            }
            t += step / 2;

            // SCLK rises (sampling)
            store.transitions[2].push((t, 1));
            t += step / 2;

            // SCLK falls
            store.transitions[2].push((t, 0));
            t += step / 2;
        }

        // CS rises
        t += step;
        store.transitions[3].push((t, 1));
        t += step;

        store.total_samples = t;

        let cfg = spi::SpiConfig {
            mosi_channel: 0,
            miso_channel: 1,
            sclk_channel: 2,
            cs_channel: 3,
            ..Default::default()
        };

        let annotations = spi::decode(&cfg, &store, 0, store.total_samples);

        // Expected: 1 MOSI annotation, 1 MISO annotation
        assert_eq!(annotations.len(), 2);
        
        let mosi_ann = annotations.iter().find(|a| a.channel == 0).unwrap();
        let miso_ann = annotations.iter().find(|a| a.channel == 1).unwrap();

        assert_eq!(mosi_ann.text, "0x55");
        assert_eq!(miso_ann.text, "0xAA");
    }
}
