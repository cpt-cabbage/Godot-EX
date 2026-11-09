def can_build(env, platform):
    env.module_add_dependencies("weather", ["tile_terrain"])
    return True


def configure(env):
    pass


def get_doc_classes():
    return ["AtmosphereEffect", "TimeOfDay3D", "WeatherDirector", "WeatherState", "WeatherTrigger3D", "WindField3D", "WindZone3D"]


def get_doc_path():
    return "doc_classes"
