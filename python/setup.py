from setuptools import setup, find_packages

setup(
    name="bugbuster",
    version="0.1.0",
    description="Python control library for the BugBuster AD74416H industrial I/O device",
    packages=find_packages(),
    python_requires=">=3.10",
    install_requires=[
        "pyserial>=3.5",
        "requests>=2.28",
    ],
    extras_require={
        "mcp": [
            "mcp>=1.0",
            "pydantic>=2.0",
        ],
    },
    entry_points={
        "console_scripts": [
            "bugbuster-mcp=bugbuster_mcp.__main__:main",
        ],
    },
)
