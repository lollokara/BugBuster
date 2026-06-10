export function parseMask(value) {
    const v = value.trim().toLowerCase();
    if (!v)
        return 0;
    const n = v.startsWith("0x") ? parseInt(v.slice(2), 16) : parseInt(v, 10);
    if (!Number.isFinite(n))
        return 0;
    return Math.max(0, Math.min(0xffff, n));
}
export function maskToHex(mask) {
    const safe = Number.isFinite(mask) ? mask : 0;
    return `0x${(safe & 0xffff).toString(16).toUpperCase().padStart(4, "0")}`;
}
export const UART_IO_MAP = [
    [1, 4], [2, 2], [3, 1],
    [4, 7], [5, 6], [6, 5],
    [7, 8], [8, 9], [9, 10],
    [10, 11], [11, 12], [12, 13],
];
export function ioLabelForGpio(gpio) {
    const entry = UART_IO_MAP.find(([, g]) => g === gpio);
    return entry ? `IO${entry[0]} (GPIO${entry[1]})` : `GPIO${gpio}`;
}
