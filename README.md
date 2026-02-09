# DeepThinkers

A summarization pipeline for meeting transcripts and lectures using Hugging Face Transformers and the MeetingBank dataset. 

## 🔍 Project Overview

This project fine-tunes the BART model for abstractive summarization using the [MeetingBank](https://huggingface.co/datasets/huuuyeah/meetingbank) dataset. It includes data preprocessing, model training, evaluation, and result visualization, all designed to support experimentation and rapid prototyping.

## 📁 Repository Structure

The key scripts are:

- `01_data_preprocessing.py` – Tokenizes the MeetingBank dataset
- `02_fine_tuning_BART.py` – Fine-tunes `facebook/bart-large-xsum` with custom settings
- `03_evaluation_and_comparison.py` – Generates summaries and evaluates performance (e.g., ROUGE)
- `UserInterface.py` - Loads a GUI to input a youtube video link and display summary
- `transcribe2.py` - Transcribes a youtube video with faster_whisper
- `summarizer.py` - Summarizes the youtube video's transcript to output to the user

## ⚖️ Dependencies & Installation
Create a virtual environment and install dependencies:
```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

`requirements.txt`:
```txt
torch
datasets
evaluate
transformers
p
tqdm
pandas
```


## 💻 How to Run

### 1. Preprocess the Dataset
```bash
python 01_data_preprocessing.py
```
This loads the MeetingBank dataset from HuggingFace, tokenizes it using the BART tokenizer, and saves it to disk.

### 2. Fine-Tune BART
```bash
python 02_fine_tuning_BART.py
```
This fine-tunes BART on the training split and logs validation loss over epochs. Hyperparameters (e.g., batch size, learning rate) can be customized.

### 3. Evaluate Results
```bash
python 03_evaluation_and_comparison.py
```
This script:
- Loads both fine-tuned and base models
- Generates summaries on the test set
- Computes ROUGE scores (ROUGE-1, ROUGE-2, ROUGE-L, ROUGE-Lsum)
- Exports a CSV with gold summaries and model outputs


## ⚛️ Metrics
We use **ROUGE** metrics via HuggingFace's `evaluate`:
- ROUGE-1: Unigram overlap
- ROUGE-2: Bigram overlap
- ROUGE-L: Longest common subsequence
- ROUGE-Lsum: Adapted for sentence-level abstraction

## 🎨 Results Interpretation
Check `evaluation_results.csv` for side-by-side comparisons. Higher ROUGE indicates better performance.

## 📝 Notebooks vs. Scripts

Originally, all functionality was developed in Jupyter notebooks. However, due to **limitations when running notebooks over SSH**, we transitioned to `.py` files for full compatibility with remote training environments (e.g., A100 instances via tmux).

## ⚙️ Hardware Requirements

This project is GPU-accelerated and was trained on an **NVIDIA A100 with 40 GB VRAM**. Training on smaller GPUs may require a lot of time.


## 📊 Visualizations

Evaluation results are available as `.csv` files and can be used for plotting:
- Heatmaps
- Parallel coordinates plots
- Word clouds
- Epoch loss curves

## ⚡ External Libraries
- Hugging Face Transformers: https://huggingface.co/docs/transformers
- Hugging Face Datasets: https://huggingface.co/docs/datasets
- Hugging Face Evaluate: https://huggingface.co/docs/evaluate

## 💻 Running Summarization Application

### Using transcribe2.exe
- Download this [zip](https://drive.google.com/file/d/1xLv_RaSsXyK5eoPzkBzKnDuWu23X0fhp/view) folder which contains 4 executables ffmpeg.exe, ffprobe.exe, transcribe2.exe, and yt-dlp.exe
    - ffmpeg.exe: Used by yt-dlp to convert YouTube video to a .wav file
    - ffprobe.exe: Gathers data for ffmpeg before audio file conversion
    - transcribe2.exe: Build of transcribe2.py, uses yt-dlp and faster_whisper to transcribe a youtube video into plain text
    - yt-dlp.exe: Needed for transcribe2 to use yt-dlp
- Alternatively, you can go to [yt-dlp documentation](https://pypi.org/project/yt-dlp/) and [yt-dlp ffmpeg auto-builds](https://github.com/yt-dlp/FFmpeg-Builds#ffmpeg-static-auto-builds) to download the respective executables
- Download UserInterface.py and summarizer.py from this repository
- Ensure that all of the executables, UserInterface.py, and summarizer.py are in the same directory
- Run UserInterface.py and paste your desired youtube video link into the text box and press the summarize button
- NOTE: The first time running transcribe.exe may take longer due to the script downloading the whisper model
- NOTE: summarizer.py currently uses a pre-trained model by default, to test better results follow the above steps to fine-tune the BART model
