Buggy -  Installation and Usage Guide

Overview

Buggy is an AI-assisted debugging tool for software projects. It supports C, C++, Java, and Python projects. The tool collects runtime information using debuggers such as GDB, Valgrind, JDB, and PDB, and then forwards the results to OpenAI for automated bug analysis and fix suggestions.

---
Development Environment

The tool was developed and tested using CLion with the WSL (Windows Subsystem for Linux) extension. This setup allows writing, building, and running the C++ tool inside a Linux environment directly from Windows.

When analyzing a project stored on the Windows filesystem, use the WSL mount path format:

./buggy /mnt/c/Users/YourName/Desktop/my-project "describe the bug" -r --gdb

---

Installation

Step 1 - Install System Dependencies

sudo apt update && sudo apt upgrade -y

sudo apt install -y \
    build-essential \
    gdb \
    valgrind \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    git \
    python3 \
    default-jdk \
    maven \
    wget \
    unzip

The following are installed automatically by the tool at runtime:

- python3-pip — installed automatically before any Python project is analyzed
- python3-venv — installed automatically when creating a virtual environment
- Gradle — the required version is downloaded automatically from services.gradle.org
- Specific Java JDK versions (8, 11, 17, 21) - installed automatically when a project requires a specific version


Step 2 - Set Up the OpenAI API Key

1. Create an account at https://platform.openai.com
2. Generate an API key under API Keys
3. Add the key permanently to your shell:

echo 'export OPENAI_API_KEY="sk-proj-YOUR_KEY_HERE"' >> ~/.bashrc
source ~/.bashrc

Verify:
echo $OPENAI_API_KEY


Step 3 — Download and Build the Tool

git clone https://github.com/bercesteyagmur/2buggAI_innoLab.git
cd 2buggAI_innoLab

make

ls -lh buggy

./buggy --help

To rebuild from scratch:

make clean
make

---
Usage

Basic Syntax

./buggy <path> <fix-description> [options]

---
Options:

--gdb — Run GDB to collect stack trace and runtime errors (C/C++)

--valgrind — Run Valgrind to detect memory leaks (C/C++)

-r — Recursively collect all source files from a directory

-v / --verbose — Show detailed output including full source code

-s — Show AI fix suggestions without automatically applying them

--json-out <file> — Save the full debugging report as a JSON file

--help — Show usage help

---
How the Tool Works

1. File Collection —> recursively scans the project and collects all source files
2. Language Detection —> detects the programming language from file extensions
3. Dependency Installation —> for Python projects, installs pip packages automatically; for Java, detects and installs the required JDK and build tool version
4. Compilation —> compiles C/C++ with gcc/g++, Java with javac/Maven/Gradle, or sets up a Python virtual environment
5. Debugging —> runs GDB or Valgrind for C/C++, JDB for Java, PDB for Python
6. Error Classification — matches output against the internal error checklist and classifies bugs by type and difficulty
7. AI Analysis —> sends the collected report to OpenAI for bug explanation and fix suggestions
8. Auto Fix —> writes AI-suggested fixes back to source files (a .bak backup is created before any change)
---
