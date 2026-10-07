def can_build(env, platform):
    env.module_add_dependencies("rig_kernel", ["noise"])
    return not env["disable_physics_3d"]


def configure(env):
    pass


def get_doc_classes():
    return [
        "GroundData",
        "RigKernel",
        "TerrainField",
        "WaterForces",
    ]


def get_doc_path():
    return "doc_classes"
