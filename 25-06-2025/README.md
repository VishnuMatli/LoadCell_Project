
# Real-Time Signal Processing and Filtering with CMSIS-DSP

This project provides a robust framework for prototyping, analyzing, and validating digital signal processing (DSP) algorithms in a Python environment. It features a **client-server architecture** where a server transmits raw signal data, and a client receives, processes, and visualizes it in real-time.

The client leverages the **Arm CMSIS-DSP** library, enabling high-performance filtering that can be directly deployed to embedded systems like **Arm Cortex-M** microcontrollers.

---

## 📚 Table of Contents

- [About The Project](#about-the-project)
- [Features](#features)
- [Required Technologies](#required-technologies)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Installation](#installation)
  - [Usage](#usage)
- [System Architecture](#system-architecture)
- [Understanding the Output](#understanding-the-output)

---

## 🔍 About The Project

This application serves as a powerful tool for **developing and testing DSP logic** in a flexible Python environment, before deploying the same CMSIS-DSP functions to resource-constrained embedded systems.

---

## ✨ Features

- **Client-Server Architecture** – Separates data acquisition from processing and visualization.
- **Real-Time Live Plotting** – Instant visual feedback using `matplotlib`.
- **High-Performance Filtering** – Utilizes `cmsisdsp` Python wrapper for efficient FIR filtering.
- **One-Time Signal Analysis** – Automatically computes peak-to-peak amplitude for raw and filtered signals.
- **Multi-Threaded Client** – Background networking for smooth, non-blocking operation.
- **Reliable Data Transfer** – Uses a robust, length-prefixed protocol to avoid data corruption.

---

## 🧰 Required Technologies

- Python 3.x
- [cmsisdsp](https://pypi.org/project/cmsisdsp/)
- numpy
- scipy
- matplotlib

---

## 🚀 Getting Started

### ✅ Prerequisites

Ensure Python 3 is installed. You can check by running:

```bash
python --version
```

### 📦 Installation

1. **Clone or Download** this repository.

2. **Install dependencies**:

```bash
pip install numpy scipy matplotlib cmsisdsp
```

3. **Upgrade `cmsisdsp`** for the latest features:

```bash
pip install --upgrade cmsisdsp
```

⚠️ **Important:** Do **not** name your script `cmsisdsp.py` to avoid conflicts with the library. Use a name like `main_client.py`.

---

## ▶️ Usage

### 🖥 Launch the Server:

- Run the server script.
- Use the interface to select a `.txt` signal data file or folder.
- Choose to send a single file or all files.

### 📡 Launch the Client:

- Open a new terminal and run the client script.
- The client will auto-connect to the server and begin processing.

### 📊 Processing and Visualization:

- The client performs one-time amplitude analysis and saves results to `analysis_results/`.
- A live plot appears showing raw and filtered signals.

### 🛑 Shutdown:

- To stop the client, **close the plot window**. This safely shuts down all processes.

---

## 🧱 System Architecture

### 🔌 Server Application

- **Role**: Data provider
- **Function**:
  - Reads `.txt` files containing raw sensor data.
  - Sends configuration (sampling interval).
  - Streams data over TCP.

### 🧠 Client Application

- **Role**: Processor and visualizer
- **Function**:
  - Connects to server and receives configuration + data.
  - Performs amplitude analysis and saves results.
  - Applies FIR filtering using `CMSIS-DSP`.
  - Simulates real-time plotting of raw vs. filtered signals.

---

## 📈 Understanding the Output

### 📝 Analysis File

- **Location**: `analysis_results/`
- **Name**: Based on input file (e.g., `analysis_data_file_01.txt`)
- **Content**:
  - Peak-to-peak amplitude of raw signal
  - Peak-to-peak amplitude of filtered signal

### 🖼 Visual Plot

Displayed via `matplotlib`:

- **Top Plot**: Raw ADC Data  
  Noisy signal received directly from server.
  
- **Bottom Plot**: FIR-Filtered Data  
  Smoothed signal using CMSIS-DSP low-pass filter. Cutoff frequency is shown in the legend.

---

## 🧪 Example Use Case

This framework is ideal for:

- Simulating **embedded DSP performance** before deployment.
- Testing **filter parameters** (like cutoff frequency or number of taps).
- Studying **real-time signal characteristics** with live feedback.

---

## 📂 Directory Structure

```
project_root/
├── server.py
├── main_client.py
├── data/
│   └── signal_data_01.txt
├── analysis_results/
│   └── analysis_signal_data_01.txt
└── README.md
```

---

## 🧠 Credits

Developed with ❤️ using CMSIS-DSP and Python. Ideal for engineers working on **real-time signal filtering** on embedded systems.

---
