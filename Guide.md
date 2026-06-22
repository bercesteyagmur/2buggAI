# IDE Setup Guide — 2buggAI with WSL

This guide covers how to set up your development environment to build and run 2buggAI on Windows using WSL (Windows Subsystem for Linux).

For general installation and usage, see [README.md](README.md).

---

## Prerequisites

- Windows 10 (version 2004+) or Windows 11
- WSL 2 with Ubuntu installed
- An OpenAI API key (see README for setup)

To install WSL with Ubuntu:

```powershell
wsl --install
```

Then follow the full dependency installation steps in [README.md](README.md).

---

## Option A: VS Code + WSL Extension

1. Install [VS Code](https://code.visualstudio.com/)
2. Install the [WSL extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-wsl)
3. Open a WSL terminal and navigate to the project:

```bash
cd ~/2buggAI
code .
```

VS Code will reopen in WSL mode. The integrated terminal runs inside Ubuntu, so `make`, `./buggy`, and all Linux tools work directly.

---

## Option B: CLion + WSL Toolchain

1. Install [CLion](https://www.jetbrains.com/clion/)
2. Go to **Settings → Build, Execution, Deployment → Toolchains**
3. Click `+` and select **WSL**
4. CLion will auto-detect the Ubuntu environment and `g++`
5. Open the project folder (Windows path — CLion handles the WSL translation)

---

## Running the tool on Windows filesystem paths

When analyzing a project stored on your Windows filesystem from inside WSL, use the WSL mount path:

```bash
./buggy /mnt/c/Users/YourName/Desktop/my-project "fix the bug" -r
```

`/mnt/c/` corresponds to your `C:\` drive. Adjust the path accordingly.

---

## Building

From your WSL terminal (or the VS Code / CLion integrated terminal in WSL mode):

```bash
git clone https://github.com/bercesteyagmur/2buggAI.git
cd 2buggAI
make
```

To rebuild from scratch:

```bash
make clean && make
```
