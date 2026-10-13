"""chamber_history: climate history kept on the chamber, served over the LAN.

One record every 5 minutes (temperature, humidity, setpoints, heater and mist duty, CO2 when a sensor is
fitted, grow phase) in a ring of 4 KiB flash sectors inside a data partition that nothing else uses, so
every owner gets weeks of charts over the LAN with or without the cloud:

  GET /history?since=<epoch>   CSV, oldest first, only records newer than `since`

The partition is found at runtime and must match one of two known layouts exactly, or the component stays
off: the stock Terrashroom table's 'storage' (0x370000, 512 KiB, ~180 days) on chambers installed over the
stock firmware, or ESPHome's 'spiffs' (60 KiB, ~20 days) on chambers flashed over USB. Partition tables
can't change over OTA, so both stay. Format, capacity and power-loss behaviour: history_log.h and
docs/device-api.md "Climate history". Host tests: esphome/tests/test_chamber_history.cpp.
"""

import esphome.codegen as cg
from esphome.components import number, sensor, time, web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import CONF_HUMIDITY, CONF_ID, CONF_TEMPERATURE, CONF_TIME_ID, PLATFORM_ESP32

CODEOWNERS = ["@triple-3-llc"]
DEPENDENCIES = ["web_server_base", "time"]

CONF_CO2 = "co2"
CONF_TEMPERATURE_SETPOINT = "temperature_setpoint"
CONF_HUMIDITY_SETPOINT = "humidity_setpoint"
CONF_HEATER_DUTY = "heater_duty"
CONF_MIST_DUTY = "mist_duty"
CONF_PHASE = "phase"

chamber_history_ns = cg.esphome_ns.namespace("chamber_history")
ChamberHistory = chamber_history_ns.class_("ChamberHistory", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ChamberHistory),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
            cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Required(CONF_TEMPERATURE): cv.use_id(sensor.Sensor),
            cv.Required(CONF_HUMIDITY): cv.use_id(sensor.Sensor),
            # Optional: the CO2 package (common/scd41.yaml) adds it.
            cv.Optional(CONF_CO2): cv.use_id(sensor.Sensor),
            cv.Required(CONF_TEMPERATURE_SETPOINT): cv.use_id(number.Number),
            cv.Required(CONF_HUMIDITY_SETPOINT): cv.use_id(number.Number),
            # 0..1, evaluated every loop and integrated over the interval.
            cv.Required(CONF_HEATER_DUTY): cv.returning_lambda,
            cv.Required(CONF_MIST_DUTY): cv.returning_lambda,
            # 0 = Climate Control off, 1 Incubation, 2 Pinning, 3 Fruiting; read at the end of each interval.
            cv.Required(CONF_PHASE): cv.returning_lambda,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
)


async def to_code(config):
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)
    cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    cg.add(var.set_temperature(await cg.get_variable(config[CONF_TEMPERATURE])))
    cg.add(var.set_humidity(await cg.get_variable(config[CONF_HUMIDITY])))
    if CONF_CO2 in config:
        cg.add(var.set_co2(await cg.get_variable(config[CONF_CO2])))
    cg.add(var.set_temperature_target(await cg.get_variable(config[CONF_TEMPERATURE_SETPOINT])))
    cg.add(var.set_humidity_target(await cg.get_variable(config[CONF_HUMIDITY_SETPOINT])))
    heater = await cg.process_lambda(config[CONF_HEATER_DUTY], [], return_type=cg.float_)
    cg.add(var.set_heater_duty(heater))
    mist = await cg.process_lambda(config[CONF_MIST_DUTY], [], return_type=cg.float_)
    cg.add(var.set_mist_duty(mist))
    phase = await cg.process_lambda(config[CONF_PHASE], [], return_type=cg.uint8)
    cg.add(var.set_phase(phase))
