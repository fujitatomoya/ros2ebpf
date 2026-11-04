from setuptools import find_packages
from setuptools import setup

package_name = 'ros2ebpf'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/' + package_name, ['package.xml']),
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
    ],
    package_data={'': ['py.typed']},
    install_requires=['ros2cli', 'pyyaml'],
    zip_safe=True,
    author='Tomoya Fujita',
    author_email='fujita.tomoya@triorb.co.jp',
    maintainer='Tomoya Fujita',
    maintainer_email='fujita.tomoya@triorb.co.jp',
    url='https://github.com/fujitatomoya/ros2ebpf',
    keywords=['ROS', 'eBPF', 'observability'],
    classifiers=[
        'Environment :: Console',
        'Intended Audience :: Developers',
        'Programming Language :: Python',
    ],
    description='The ebpf command for ROS 2 command line tools: control and query ros2ebpfd.',
    long_description="""\
The package provides the `ros2 ebpf` command, which talks to the ros2ebpfd daemon over its
control socket (or ssh / HTTP for other hosts of a robot). It never creates a DDS participant.""",
    license='Apache-2.0',
    extras_require={'test': ['pytest']},
    entry_points={
        'ros2cli.command': [
            'ebpf = ros2ebpf.command.ebpf:EbpfCommand',
        ],
        'ros2cli.extension_point': [
            'ros2ebpf.verb = ros2ebpf.verb:VerbExtension',
        ],
        'ros2ebpf.verb': [
            'status = ros2ebpf.verb.status:StatusVerb',
            'enable = ros2ebpf.verb.enable:EnableVerb',
            'disable = ros2ebpf.verb.disable:DisableVerb',
            'reload = ros2ebpf.verb.reload:ReloadVerb',
            'top = ros2ebpf.verb.top:TopVerb',
            'config = ros2ebpf.verb.config:ConfigVerb',
            'expectations = ros2ebpf.verb.expectations:ExpectationsVerb',
            'doctor = ros2ebpf.verb.doctor:DoctorVerb',
            'metrics = ros2ebpf.verb.metrics:MetricsVerb',
        ],
    }
)
