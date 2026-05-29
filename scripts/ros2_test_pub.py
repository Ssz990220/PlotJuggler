#!/usr/bin/env python3.10
"""Publishes test timeseries on /pj/{sin,cos,ramp} at 20 Hz.
Run:  python3.10 scripts/ros2_test_pub.py
"""
import sys
sys.path = [
    "/opt/ros/humble/local/lib/python3.10/dist-packages",
    "/opt/ros/humble/lib/python3.10/site-packages",
    "/usr/lib/python3.10",
    "/usr/lib/python3.10/lib-dynload",
]

import math
import rclpy
from rclpy.node import Node
from std_msgs.msg import Float64

RATE_HZ = 20
DT = 1.0 / RATE_HZ


class TestPublisher(Node):
    def __init__(self):
        super().__init__("pj4_test_publisher")
        self.sin_pub  = self.create_publisher(Float64, "/pj/sin",  10)
        self.cos_pub  = self.create_publisher(Float64, "/pj/cos",  10)
        self.ramp_pub = self.create_publisher(Float64, "/pj/ramp", 10)
        self.timer = self.create_timer(DT, self._tick)
        self._t = 0.0
        self.get_logger().info("Publishing /pj/{sin,cos,ramp} at 20 Hz")

    def _tick(self):
        def send(pub, value):
            msg = Float64()
            msg.data = value
            pub.publish(msg)

        send(self.sin_pub,   5.0 * math.sin(2 * math.pi * 0.5 * self._t))
        send(self.cos_pub,   5.0 * math.cos(2 * math.pi * 0.5 * self._t))
        send(self.ramp_pub,  (self._t % 5.0) * 2.0)
        self._t += DT


def main():
    rclpy.init()
    node = TestPublisher()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
