// Zigbee2MQTT external converter for the Awning Wind Protection Sensor (spec §11).
//
// ALTERNATE HOST PATH. Use this ONLY if the ConBee II runs Zigbee2MQTT. For a
// ConBee II running deCONZ (the current setup), use integration/deconz/ instead.
// A single ConBee II cannot run both deCONZ and Z2M at once. The firmware is
// identical either way — only the host-side mapping differs.
//
// The firmware exposes four ZigbeeAnalog endpoints (Analog Input cluster 0x000C)
// which Z2M would otherwise surface as anonymous "analog" values. This converter
// gives each a named, unit-tagged expose.
//
// Install: copy to your Z2M `data/external_converters/` folder and restart Z2M.
// Then adjust `zigbeeModel`/`manufacturerName` below to match what the device
// actually reports (check the device's "About" tab in the Z2M frontend).
//
// TODO(hardware): fill zigbeeModel / manufacturerName from the joined device.

const {deviceEndpoints, numeric} = require('zigbee-herdsman-converters/lib/modernExtend');

const analog = (name, ep, unit) => numeric({
    name,
    endpointNames: [String(ep)],
    cluster: 'genAnalogInput',
    attribute: 'presentValue',
    reporting: {min: 0, max: 3600, change: 1},
    unit,
    access: 'STATE_GET',
    description: `${name} (${unit})`,
});

module.exports = [{
    zigbeeModel: ['ESP32H2.Awning'],           // TODO: match reported model
    model: 'awning-wind-sensor',
    vendor: 'DIY',
    description: 'Awning movement sensor (ADXL345 + ESP32-H2 Zigbee)',
    extend: [
        deviceEndpoints({endpoints: {'10': 10, '11': 11, '12': 12, '13': 13}}),
        analog('movement_rms',  10, 'mg'),
        analog('movement_peak', 11, 'mg'),
        analog('tilt_pitch',    12, 'deg'),
        analog('tilt_roll',     13, 'deg'),
    ],
    // Battery arrives via the standard Power Configuration cluster and needs no
    // custom mapping — Z2M exposes it automatically.
}];
