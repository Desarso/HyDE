#!/bin/bash

# --- Configuration (No longer needed, but kept for clarity) ---
# RECORDING_FILE="$HOME/.config/waybar/recording_status"  # No longer used
# PROCESSING_FILE="$HOME/.config/waybar/processing_status" # No longer used
# --------------------------------------------------

# Function to create the JSON output (remains the same)
output_json() {
  local text="$1"
  local class="$2"
  local tooltip="$3"

  echo "{\"text\": \"$text\", \"class\": \"$class\", \"tooltip\": \"$tooltip\"}"
}

# --- Main Logic (Simplified) ---

if [[ -f "/tmp/recording.pid" ]]; then
  output_json "● REC" "recording" "Recording Active"
elif [[ -f "/tmp/processing.pid" ]]; then
  output_json "● PROC" "processing" "Processing Data"
else
  output_json "▶" "inactive" "Start Recording"
fi

exit 0
