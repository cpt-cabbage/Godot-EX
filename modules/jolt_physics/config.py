def can_build(env, platform):
    return not env["disable_physics_3d"]


def configure(env):
    pass


def get_doc_classes():
    return [
        "JoltPhysicsServer3D",
    ]


def get_doc_path():
    return "doc_classes"
