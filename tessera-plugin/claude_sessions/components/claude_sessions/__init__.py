"""Claude sessions: the ESPHome side. The four homeassistant text sensors and the two switches of plugin.yaml are
handed to the component; smart_display.register_plugin() reads the id, version, tile memory and the screen's texts
from the manifest and translations/ beside this folder (docs/FIRMWARE_API.md of the plugins repository)."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import smart_display, switch, text_sensor
from esphome.const import CONF_ID

DEPENDENCIES = ["smart_display"]

CONF_EVENT = "event"
CONF_EVENT_REV = "event_rev"
CONF_OPEN = "open"
CONF_REV = "rev"
CONF_QUESTION_POPUP = "question_popup"
CONF_HINT_POPUP = "hint_popup"

claude_sessions_ns = cg.esphome_ns.namespace("claude_sessions")
ClaudeSessions = claude_sessions_ns.class_("ClaudeSessions", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ClaudeSessions),
        cv.Required(CONF_EVENT): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_EVENT_REV): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_OPEN): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_REV): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_QUESTION_POPUP): cv.use_id(switch.Switch),
        cv.Required(CONF_HINT_POPUP): cv.use_id(switch.Switch),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
    cg.add(var.set_event(await cg.get_variable(config[CONF_EVENT])))
    cg.add(var.set_event_rev(await cg.get_variable(config[CONF_EVENT_REV])))
    cg.add(var.set_open(await cg.get_variable(config[CONF_OPEN])))
    cg.add(var.set_rev(await cg.get_variable(config[CONF_REV])))
    cg.add(var.set_question_popup(await cg.get_variable(config[CONF_QUESTION_POPUP])))
    cg.add(var.set_hint_popup(await cg.get_variable(config[CONF_HINT_POPUP])))
