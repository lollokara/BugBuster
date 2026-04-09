
export const tauriMock = () => {
  (window as any).__TAURI__ = {
    core: {
      invoke: async (cmd: string, args: any) => {
        console.log(`[Tauri Mock] invoke: ${cmd}`, args);
        switch (cmd) {
          case 'la_check_usb':
            return true;
          
          case 'la_get_status':
            return {
              state: 3, // DONE
              channels: 4,
              samplesCaptured: 100000,
              totalSamples: 100000,
              actualRateHz: 100000000,
            };

          case 'la_get_capture_info':
            return {
              channels: 4,
              sampleRateHz: 100000000,
              totalSamples: 100000,
              durationSec: 0.001,
              triggerSample: 50000,
            };

          case 'la_load_raw':
            return 100000;

          case 'la_get_view': {
            const { startSample, endSample } = args;
            const totalSamples = 100000;
            const transitions = [
              [[0, 0], [1000, 1], [2000, 0], [3000, 1], [4000, 0], [5000, 1]], // Ch 0: some toggles
              [[0, 1], [5000, 0], [10000, 1]],                               // Ch 1: slow toggle
              [],                                                              // Ch 2
              []                                                               // Ch 3
            ];
            // Filter transitions for the requested range
            const filteredTransitions = transitions.map(chTrans => 
              chTrans.filter(t => t[0] >= startSample && t[0] <= endSample)
            );
            return {
              channels: 4,
              sampleRateHz: 100000000,
              totalSamples: totalSamples,
              viewStart: startSample,
              viewEnd: endSample,
              triggerSample: 50000,
              channelTransitions: filteredTransitions,
              density: [],
              decimated: false
            };
          }

          case 'la_decode': {
            const { config, startSample, endSample } = args;
            const protocol = config.type;
            const anns = [];
            if (protocol === 'uart') {
                anns.push(
                    { channel: config.txChannel, row: 0, startSample: 1000, endSample: 2000, text: 'H', annType: 'data' },
                    { channel: config.txChannel, row: 0, startSample: 3000, endSample: 4000, text: 'e', annType: 'data' },
                    { channel: config.txChannel, row: 0, startSample: 5000, endSample: 6000, text: 'l', annType: 'data' },
                    { channel: config.txChannel, row: 0, startSample: 7000, endSample: 8000, text: 'l', annType: 'data' },
                    { channel: config.txChannel, row: 0, startSample: 9000, endSample: 10000, text: 'o', annType: 'data' }
                );
            } else if (protocol === 'i2c') {
                anns.push(
                    { channel: config.sdaChannel, row: 0, startSample: 1000, endSample: 1100, text: 'S', annType: 'control' },
                    { channel: config.sdaChannel, row: 0, startSample: 1200, endSample: 2000, text: '0x3C', annType: 'address' },
                    { channel: config.sdaChannel, row: 0, startSample: 2100, endSample: 2200, text: 'W', annType: 'control' },
                    { channel: config.sdaChannel, row: 0, startSample: 2300, endSample: 2400, text: 'A', annType: 'control' }
                );
            } else if (protocol === 'spi') {
                anns.push(
                    { channel: config.mosiChannel, row: 0, startSample: 1000, endSample: 2000, text: '0xAA', annType: 'data' },
                    { channel: config.misoChannel, row: 1, startSample: 1000, endSample: 2000, text: '0x55', annType: 'data' }
                );
            }
            return anns.filter(a => a.endSample >= startSample && a.startSample <= endSample);
          }

          case 'la_arm':
          case 'la_stop':
          case 'la_force':
          case 'la_configure':
            return null;

          default:
            console.warn(`[Tauri Mock] Unhandled command: ${cmd}`);
            return null;
        }
      }
    },
    event: {
      listen: async (event: string, handler: (e: any) => void) => {
        console.log(`[Tauri Mock] listen: ${event}`);
        return () => { console.log(`[Tauri Mock] unlisten: ${event}`); };
      }
    }
  };
};
