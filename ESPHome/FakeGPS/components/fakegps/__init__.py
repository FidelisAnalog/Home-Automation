import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import time as time_
from esphome.const import CONF_ID, CONF_TIME_ID
from esphome.core import TimePeriod

CODEOWNERS = ["@FidelisAnalog"]
DEPENDENCIES = ["time"]

fakegps_ns = cg.esphome_ns.namespace("fakegps")
FakeGPS = fakegps_ns.class_("FakeGPS", cg.Component)

Parity = fakegps_ns.enum("Parity")
PARITIES = {
    "none": Parity.PARITY_NONE,
    "even": Parity.PARITY_EVEN,
    "odd": Parity.PARITY_ODD,
}

TimeBasis = fakegps_ns.enum("TimeBasis")
TIME_BASES = {
    "local": TimeBasis.BASIS_LOCAL,
    "utc": TimeBasis.BASIS_UTC,
}

MotionMode = fakegps_ns.enum("MotionMode")
MOTION_MODES = {
    "auto": MotionMode.MODE_AUTO,
    "on": MotionMode.MODE_ON,
    "off": MotionMode.MODE_OFF,
}

CONF_BAUD = "baud"
CONF_INVERT = "invert"
CONF_DATA_BITS = "data_bits"
CONF_STOP_BITS = "stop_bits"
CONF_PARITY = "parity"
CONF_EN_GPZDA = "en_gpzda"
CONF_EN_GPRMC = "en_gprmc"
CONF_EN_GPGGA = "en_gpgga"
CONF_EN_GPGSA = "en_gpgsa"
CONF_LATITUDE = "latitude"
CONF_LONGITUDE = "longitude"
CONF_SATS = "sats"
CONF_ALTITUDE_M = "altitude_m"
CONF_HDOP = "hdop"
CONF_TIME_BASIS = "time_basis"
CONF_TIME_OFFSET_MS = "time_offset_ms"
CONF_SENTENCE_INTERVAL = "sentence_interval"
CONF_OFF_DELAY = "off_delay"
CONF_RESTROBE_PERIOD = "restrobe_period"
CONF_MOTION_MODE = "motion_mode"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FakeGPS),
        cv.GenerateID(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        cv.Optional(CONF_BAUD, default=9600): cv.int_range(min=300, max=115200),
        cv.Optional(CONF_INVERT, default=False): cv.boolean,
        cv.Optional(CONF_DATA_BITS, default=8): cv.one_of(7, 8, int=True),
        cv.Optional(CONF_STOP_BITS, default=1): cv.one_of(1, 2, int=True),
        cv.Optional(CONF_PARITY, default="none"): cv.enum(PARITIES, lower=True),
        cv.Optional(CONF_EN_GPZDA, default=False): cv.boolean,
        cv.Optional(CONF_EN_GPRMC, default=True): cv.boolean,
        cv.Optional(CONF_EN_GPGGA, default=False): cv.boolean,
        cv.Optional(CONF_EN_GPGSA, default=False): cv.boolean,
        cv.Optional(CONF_LATITUDE, default=40.2333): cv.float_range(min=-90, max=90),
        cv.Optional(CONF_LONGITUDE, default=-82.85): cv.float_range(min=-180, max=180),
        cv.Optional(CONF_SATS, default=10): cv.int_range(min=4, max=12),
        cv.Optional(CONF_ALTITUDE_M, default=100.0): cv.float_,
        cv.Optional(CONF_HDOP, default=0.9): cv.positive_float,
        cv.Optional(CONF_TIME_BASIS, default="local"): cv.enum(TIME_BASES, lower=True),
        cv.Optional(CONF_TIME_OFFSET_MS, default=0): cv.int_range(min=-1000, max=1000),
        cv.Optional(CONF_SENTENCE_INTERVAL, default="5s"): cv.All(
            cv.positive_time_period_seconds,
            cv.Range(min=TimePeriod(seconds=1), max=TimePeriod(seconds=3600)),
        ),
        cv.Optional(CONF_OFF_DELAY, default="15min"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_RESTROBE_PERIOD, default="1s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=TimePeriod(seconds=1), max=TimePeriod(seconds=15)),
        ),
        cv.Optional(CONF_MOTION_MODE, default="auto"): cv.enum(MOTION_MODES, lower=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    rtc = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time_source(rtc))

    cg.add(var.set_baud(config[CONF_BAUD]))
    cg.add(var.set_invert(config[CONF_INVERT]))
    cg.add(var.set_data_bits(config[CONF_DATA_BITS]))
    cg.add(var.set_stop_bits(config[CONF_STOP_BITS]))
    cg.add(var.set_parity(config[CONF_PARITY]))
    cg.add(var.set_en_gpzda(config[CONF_EN_GPZDA]))
    cg.add(var.set_en_gprmc(config[CONF_EN_GPRMC]))
    cg.add(var.set_en_gpgga(config[CONF_EN_GPGGA]))
    cg.add(var.set_en_gpgsa(config[CONF_EN_GPGSA]))
    cg.add(var.set_latitude(config[CONF_LATITUDE]))
    cg.add(var.set_longitude(config[CONF_LONGITUDE]))
    cg.add(var.set_sats(config[CONF_SATS]))
    cg.add(var.set_altitude(config[CONF_ALTITUDE_M]))
    cg.add(var.set_hdop(config[CONF_HDOP]))
    cg.add(var.set_time_basis(config[CONF_TIME_BASIS]))
    cg.add(var.set_time_offset_ms(config[CONF_TIME_OFFSET_MS]))
    cg.add(var.set_sentence_interval_s(config[CONF_SENTENCE_INTERVAL]))
    cg.add(var.set_off_delay_ms(config[CONF_OFF_DELAY]))
    cg.add(var.set_restrobe_period_ms(config[CONF_RESTROBE_PERIOD]))
    cg.add(var.set_motion_mode(config[CONF_MOTION_MODE]))
