# Real-Time Panoramic Tracking for Event Cameras


## Yunfan
I modified this repository in order to match my need for the master thesis.
Be careful that to compile this project, you need specific gcc and cuda version.
The modifications are as below:
- add support to 128*128 DVS camera
- output estimation pose to txt file
  - format: time x y z q_x q_y q_z q_w
- change its projection function; details about this change can be found in my thesis Section 4.2
- update undistortion function to support more datasets
- modify GUI to support continuous tracking between datasets 
  - some datasets like poster_rotation is too large to load in one time. so I separate it into several smaller files and use this feature to get the result.
- add timer to count the time consumption on each datasets
- add upscale selection in GUI


This repository provides software for our publication "Real-Time Panoramic Tracking for Event Cameras", ICCP 2017.

If you use this code please cite the following publication (https://arxiv.org/abs/1703.05161):
~~~
@inproceedings{reinbacher_iccp2017,
  author = {Christian Reinbacher and Gottfried Munda and Thomas Pock},
  title = {{Real-Time Panoramic Tracking for Event Cameras}},
  booktitle = {2017 International Conference on Computational Photography (ICCP)},
  year = {2017},
}
~~~

## Compiling
For your convenience, the required libraries that are on Github are added as
submodules. So clone this repository with `--recursive` or do a
~~~
git submodule update --init --recursive
~~~
after cloning.

This software requires:
 - GCC >= 4.9
 - CMake >= 3.2
 - Qt >= 5.6
 - ImageUtilities (https://github.com/VLOGroup/imageutilities) with the `iugui`, `iuio` and `iumath` modules
 - libcaer >=2.0 (https://github.com/inilabs/libcaer)
 - cnpy (https://github.com/rogersce/cnpy)
 - DVS128 camera (can also load events from files)

To compile, first build and install ImageUtilities, then:
 ~~~
cd cnpy
cmake .
make
(sudo) make install
cd ../libcaer
cmake .
make
(sudo) make install
cd ..
mkdir build
cd build
cmake ../src
make -j6
 ~~~

 Per default, the application will compile to support the iniLabs DVS128.

## Usage
Launch `live_tracking_gui <camera_calibration_file.txt>` to get to the main application which should look like this:
<img src="https://github.com/VLOGroup/dvs-panotracking/raw/master/images/screenshot.png"></img>

Camera calibration files are included in the `data/` folder, the format is as follows:
~~~
<3x3 camera intrinsics matrix>
<width of panorama>
<height of panorama>
<radial distortion coefficient>
~~~

Clicking on the play button with an attached camera will start the live reconstruction method. Alternatively, events can be loaded from text files with one event per line:
~~~
<timestamp in seconds> <x> <y> <polarity (-1/1)>
...
...
~~~

If you don't own a camera, there is sample data available in the `data/` directory. Simply extract it, load it in the application and press the play button.
