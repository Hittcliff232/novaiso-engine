import math


def on_spawn(entity, scene):
    entity.set_float("phase", 0.0)


def on_update(entity, scene, dt):
    phase = entity.get_float("phase", 0.0) + dt * 2.2
    entity.set_float("phase", phase)
    glow = 0.7 + 0.3 * math.sin(phase)
    entity.set_tint(0.8 + 0.2 * glow, 0.55 + 0.1 * glow, 0.35, 1.0)
