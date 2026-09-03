# Copyright 2026 Bloomberg Finance L.P.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Tests for the Python project metadata."""

import sys
from importlib import import_module
from pathlib import Path

import pytest

if sys.version_info < (3, 11):
    pytest.skip("tomllib requires Python 3.11 or newer", allow_module_level=True)

tomllib = import_module("tomllib")


PYTHON_DIR = Path(__file__).resolve().parents[3]


def _requirements(path):
    """Return installable requirements from a requirements file."""
    return [
        line
        for line in path.read_text().splitlines()
        if line.strip()
        and not line.lstrip().startswith("#")
        and not line.lstrip().startswith("-r")
    ]


def test_project_dependencies_match_requirements():
    """Project dependencies mirror requirements.txt."""
    with (PYTHON_DIR / "pyproject.toml").open("rb") as project_file:
        project = tomllib.load(project_file)

    assert project["project"]["dependencies"] == _requirements(
        PYTHON_DIR / "requirements.txt"
    )


def test_test_dependencies_match_requirements():
    """Test extras mirror requirements-test.txt."""
    with (PYTHON_DIR / "pyproject.toml").open("rb") as project_file:
        project = tomllib.load(project_file)

    assert project["project"]["optional-dependencies"]["test"] == _requirements(
        PYTHON_DIR / "requirements-test.txt"
    )


def test_dev_dependencies_match_requirements():
    """Development extras mirror requirements-dev.txt."""
    with (PYTHON_DIR / "pyproject.toml").open("rb") as project_file:
        project = tomllib.load(project_file)

    assert project["project"]["optional-dependencies"]["dev"] == _requirements(
        PYTHON_DIR / "requirements-dev.txt"
    )
