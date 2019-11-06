import bpy
from bpy.props import *
from .. base import SimulationNode
from .. node_builder import NodeBuilder


class ChangeParticleColorNode(bpy.types.Node, SimulationNode):
    bl_idname = "fn_ChangeParticleColorNode"
    bl_label = "Change Color"

    def declaration(self, builder: NodeBuilder):
        builder.fixed_input("color", "Color", "Color")
        builder.execute_output("execute", "Execute")


class ChangeParticleVelocityNode(bpy.types.Node, SimulationNode):
    bl_idname = "fn_ChangeParticleVelocityNode"
    bl_label = "Change Velocity"

    mode: EnumProperty(
        name="Mode",
        items=[
            ('SET', "Set", "Set a specific velocity", 'NONE', 0),
            ('RANDOMIZE', "Randomize", "Apply some randomization to the velocity", 'NONE', 1),
        ],
        update= SimulationNode.sync_tree,
    )

    def declaration(self, builder: NodeBuilder):
        if self.mode == 'SET':
            builder.fixed_input("velocity", "Velocity", "Vector")
        elif self.mode == 'RANDOMIZE':
            builder.fixed_input("randomness", "Randomness", "Float", default=0.5)

        builder.execute_output("execute", "Execute")

    def draw(self, layout):
        layout.prop(self, "mode", text="")


class ChangeParticleSizeNode(bpy.types.Node, SimulationNode):
    bl_idname = "fn_ChangeParticleSizeNode"
    bl_label = "Change Size"

    def declaration(self, builder: NodeBuilder):
        builder.fixed_input("size", "Size", "Float", default=0.01)
        builder.execute_output("execute", "Execute")


class ChangeParticlePosition(bpy.types.Node, SimulationNode):
    bl_idname = "fn_ChangeParticlePositionNode"
    bl_label = "Change Position"

    def declaration(self, builder: NodeBuilder):
        builder.fixed_input("position", "Position", "Vector")
        builder.execute_output("execute", "Execute")
