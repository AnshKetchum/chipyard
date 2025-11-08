#!/usr/bin/env bash
set -e  # Exit immediately if any command fails

# Install GDS viewers
pip install gdspy

# Channel setup
conda config --set channel_priority true
conda config --add channels defaults

# Download all files for Sky130A PDK
conda create -y -c litex-hub --prefix ~/.conda-sky130 open_pdks.sky130a=1.0.457_0_g32e8f23

# Clone the SRAM22 Sky130 SRAM macros
git clone https://github.com/AnshKetchum/sram22_sky130_macros ~/sram22_sky130_macros

# Enter the cloned directory and run unzip.sh
pushd ~/sram22_sky130_macros
bash unzip.sh
popd

# Install all VLSI tools
conda create -y -c litex-hub --prefix ~/.conda-yosys yosys=0.27_4_gb58664d44
conda create -y -c litex-hub --prefix ~/.conda-openroad openroad=2.0_7070_g0264023b6
conda create -y -c litex-hub --prefix ~/.conda-klayout klayout=0.28.5_98_g87e2def28
conda create -y -c litex-hub --prefix ~/.conda-signoff magic=8.3.376_0_g5e5879c netgen=1.5.250_0_g178b172

# Restore strict channel priority
conda config --set channel_priority strict
