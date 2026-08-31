def can_build(env, platform):
    # OpenColorIO is only useful where the RenderingDevice-based renderers run,
    # since the colour-managed output transform is a RenderingDevice pass.
    return env["vulkan"] or env["d3d12"] or env["metal"]


def configure(env):
    pass


def get_doc_classes():
    return [
        "OCIOServer",
    ]


def get_doc_path():
    return "doc_classes"
