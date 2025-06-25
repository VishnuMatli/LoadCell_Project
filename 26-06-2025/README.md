
# 📡 Real-Time DSP Signal Processing with CMSIS-DSP (Python Client)

This project demonstrates real-time digital signal processing (DSP) of ADC data using Python and the Arm CMSIS-DSP library. It includes live data reception, FIR filtering, DC offset handling, and real-time plotting using Matplotlib — ideal for prototyping signal filtering and embedded data visualization.

---

## 🛠️ Features

- 📥 Real-time TCP data reception from a server
- 🔃 FIR low-pass filtering (via CMSIS-DSP with SciPy filter design)
- 📏 DC offset removal and amplitude analysis
- 📊 Live Matplotlib plotting (raw + filtered)
- 🕹️ Speed control (1ms, 20ms...) via GUI radio buttons
- 💾 Automatic result logging and clean shutdown

---

## 🧱 Project Structure

```
project/
│
├── core_dsp.py            # Core filtering, amplitude, DC offset logic
├── gui_plot.py            # Real-time GUI plotting and widgets
├── file_io.py             # Result logging and output management
├── network_client.py      # TCP socket reception in a background thread
├── main.py                # Main entry point, orchestrates the app
└── analysis_results/      # Stores analysis logs (.txt)
```

---

## 🧪 Quick Start

```bash
pip install numpy scipy matplotlib
python main.py
```

---

## ⚙️ Requirements

- Python 3.8+
- Libraries: `numpy`, `scipy`, `matplotlib`
- Optional: CMSIS-DSP Python bindings (if integrated with native builds)

---

## 💡 How It Works

### ✔️ One-Time Signal Analysis
- Extracts DC offset
- Computes raw & filtered amplitudes
- Saves analysis to `analysis_results/`

### 🔄 Real-Time Loop
- Adds new data points at GUI-selected interval
- Applies FIR filtering
- Updates dual-line plot live (raw + filtered)

---

## 📊 Output Example

Each data file generates:
- 📈 A real-time dual plot (Raw vs Filtered)
- 📝 A text report:
  ```
  File: 535g20250502pm427ms20hz0.txt
  Raw Amplitude: 138.7 g
  Filtered Amplitude: 116.3 g
  DC Offset: 2048.5
  Timestamp: 2025-06-26 14:32:18
  ```

---

## 🤝 Contributing

We welcome contributions!

### 🧩 Ways to Contribute

- 💡 Suggest feature improvements (e.g., FFT analysis, CSV export)
- 🐞 Report bugs and issues
- 🧪 Add unit tests for DSP functions
- ✍️ Improve documentation or examples
- 🎨 Enhance GUI appearance or interactivity

### 📌 Contribution Guidelines

1. **Fork** the repo
2. **Clone** your fork
   ```bash
   git clone https://github.com/VishnuMatli/LoadCell_Project.git
   ```
3. **Create a feature branch**
   ```bash
   git checkout -b feature/new-filter-option
   ```
4. **Commit and push**
5. **Open a Pull Request**

Please follow **PEP8 style**, write clear commit messages, and test your changes.

---

## 🐛 Reporting Issues

Use the [Issues tab](https://github.com/VishnuMatli/LoadCell_Project/issues) to report:

- Incorrect filtering behavior
- Plotting bugs
- Network/connection issues
- Documentation typos or enhancements

**Include screenshots and logs** where possible to help us debug!

---

## 📃 License

This project is licensed under the MIT License. Feel free to use and modify it in your own applications.

---

## 👨‍💻 Author

**Vishnu Matli**  
B.Tech – Computer Science (Networks)  
Certified IoT, DSP, and Cybersecurity enthusiast  
📫 [LinkedIn](https://www.linkedin.com/in/vishnu-matli)
