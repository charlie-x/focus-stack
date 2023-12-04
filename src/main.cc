#include <iostream>
#include "options.hh"
#include "focusstack.hh"
#include <opencv2/core.hpp>
#include <filesystem>

#ifndef GIT_VERSION
#define GIT_VERSION "unknown"
#endif

using namespace focusstack;

/**
 * @brief Retrieves a list of JPEG and PNG files from a specified folder.
 *
 * This function searches the given directory for all files with .jpg or .png
 * extensions and returns their names in a std::vector.
 *
 * @param folderName The name of the folder to search.
 * @return std::vector<std::string> A vector containing the full path and filename.
 */
std::vector<std::string> find_files(const std::string& folderName) {
	std::vector<std::string> filepaths;

	for (const auto& entry : std::filesystem::directory_iterator(folderName)) {
		auto path = entry.path();
		auto ext = path.extension().string();

		if (ext == ".jpg" || ext == ".png") {
			filepaths.push_back(path.string());
		}
	}

	return filepaths;
}


int main(int argc, const char *argv[])
{
  Options options(argc, argv);
  FocusStack stack;

  if (options.has_flag("--version"))
  {
    std::cerr << "focus-stack " GIT_VERSION ", built " __DATE__ " " __TIME__ "\n"
                 "Compiled with OpenCV version " CV_VERSION "\n"
                 "Copyright (c) 2019 Petteri Aimonen\n\n"

"Permission is hereby granted, free of charge, to any person obtaining a copy\n"
"of this software and associated documentation files (the \"Software\"), to\n"
"deal in the Software without restriction, including without limitation the\n"
"rights to use, copy, modify, merge, publish, distribute, sublicense, and/or\n"
"sell copies of the Software, and to permit persons to whom the Software is\n"
"furnished to do so, subject to the following conditions:\n\n"

"The above copyright notice and this permission notice shall be included in all\n"
"copies or substantial portions of the Software.\n\n"

"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
"IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
"FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
"AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
"LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
"OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
"SOFTWARE."
              << std::endl;
    return 0;
  }

  if (options.has_flag("--opencv-version"))
  {
    std::cerr << cv::getBuildInformation().c_str() << std::endl;
    return 0;
  }

  if (options.has_flag("--help") || (!options.has_flag("--input-folder")  && options.get_filenames().size() < 2 ) )
  {
    std::cerr << "Usage: " << argv[0] << " [options] file1.jpg file2.jpg ...\n";
    std::cerr << "\n";
	std::cerr << "Input file options:\n"
		"  --input-folder=<folder>           Set input folder to add from\n";

	std::cerr << "Output file options:\n"
                 "  --output=output.jpg           Set output filename\n"
                 "  --depthmap=depthmap.png       Write a depth map image (default disabled)\n"
                 "  --3dview=3dview.png           Write a 3D preview image (default disabled)\n"
                 "  --save-steps                  Save intermediate images from processing steps\n"
                 "  --jpgquality=95               Quality for saving in JPG format (0-100, default 95)\n"
                 "  --nocrop                      Save full image, including extrapolated border data\n";
    std::cerr << "\n";
    std::cerr << "Image alignment options:\n"
                 "  --reference=0                 Set index of image used as alignment reference (default middle one)\n"
                 "  --global-align                Align directly against reference (default with neighbour image)\n"
                 "  --full-resolution-align       Use full resolution images in alignment (default max 2048 px)\n"
                 "  --no-whitebalance             Don't attempt to correct white balance differences\n"
                 "  --no-contrast                 Don't attempt to correct contrast and exposure differences\n"
                 "  --align-only                  Only align the input image stack and exit\n"
                 "  --align-keep-size             Keep original image size by not cropping alignment borders\n";
    std::cerr << "\n";
    std::cerr << "Image merge options:\n"
                 "  --consistency=2               Neighbour pixel consistency filter level 0..2 (default 2)\n"
                 "  --denoise=1.0                 Merged image denoise level (default 1.0)\n";
    std::cerr << "\n";
    std::cerr << "Depth map generation options:\n"
                 "  --depthmap-threshold=10       Threshold to accept depth points (0-255, default 10)\n"
                 "  --depthmap-smooth-xy=20       Smoothing of depthmap in X and Y directions (default 20)\n"
                 "  --depthmap-smooth-z=40        Smoothing of depthmap in Z direction (default 40)\n"
                 "  --remove-bg=0                 Positive value removes black background, negative white\n"
                 "  --halo-radius=20              Radius of halo effects to remove from depthmap\n"
                 "  --3dviewpoint=x:y:z:zscale    Viewpoint for 3D view (default 1:1:1:2)\n";
    std::cerr << "\n";
    std::cerr << "Performance options:\n"
                 "  --threads=2                   Select number of threads to use (default number of CPUs + 1)\n"
                 "  --batchsize=8                 Images per merge batch (default 8)\n"
                 "  --no-opencl                   Disable OpenCL GPU acceleration (default enabled)\n"
                 "  --wait-images=0.0             Wait for image files to appear (allows simultaneous capture and processing)\n";
    std::cerr << "\n";
    std::cerr << "Information options:\n"
                 "  --verbose                     Verbose output from steps\n"
                 "  --version                     Show application version number\n"
                 "  --opencv-version              Show OpenCV library version and build info\n";
    return 1;
  }

  
  // added --input-folder option, scans the dir for jpg/pngs
  if (options.has_flag("--input-folder"))
  {
	  std::vector<std::string>file_list;
	  file_list = find_files( options.get_arg("--input-folder", ".") );
	  stack.set_inputs( file_list);

  } else {

	  stack.set_inputs(options.get_filenames());
  }

  // Output file options
  stack.set_output(options.get_arg("--output", "output.jpg"));
  stack.set_depthmap(options.get_arg("--depthmap", ""));
  stack.set_3dview(options.get_arg("--3dview", ""));
  stack.set_jpgquality(std::stoi(options.get_arg("--jpgquality", "95")));
  stack.set_save_steps(options.has_flag("--save-steps"));
  stack.set_nocrop(options.has_flag("--nocrop"));

  // Image alignment options
  int flags = FocusStack::ALIGN_DEFAULT;
  if (options.has_flag("--global-align"))             flags |= FocusStack::ALIGN_GLOBAL;
  if (options.has_flag("--full-resolution-align"))    flags |= FocusStack::ALIGN_FULL_RESOLUTION;
  if (options.has_flag("--no-whitebalance"))          flags |= FocusStack::ALIGN_NO_WHITEBALANCE;
  if (options.has_flag("--no-contrast"))              flags |= FocusStack::ALIGN_NO_CONTRAST;
  if (options.has_flag("--align-keep-size"))          flags |= FocusStack::ALIGN_KEEP_SIZE;
  stack.set_align_flags(flags);

  if (options.has_flag("--reference"))
  {
    stack.set_reference(std::stoi(options.get_arg("--reference")));
  }

  if (options.has_flag("--align-only"))
  {
    stack.set_align_only(true);
    stack.set_output(options.get_arg("--output", "aligned_"));
  }

  // Image merge options
  stack.set_consistency(std::stoi(options.get_arg("--consistency", "2")));
  stack.set_denoise(std::stof(options.get_arg("--denoise", "1.0")));

  // Depth map generation options
  stack.set_depthmap_smooth_xy(std::stof(options.get_arg("--depthmap-smooth-xy", "20")));
  stack.set_depthmap_smooth_z(std::stof(options.get_arg("--depthmap-smooth-z", "40")));
  stack.set_depthmap_threshold(std::stoi(options.get_arg("--depthmap-threshold", "10")));
  stack.set_halo_radius(std::stof(options.get_arg("--halo-radius", "20")));
  stack.set_remove_bg(std::stoi(options.get_arg("--remove-bg", "0")));
  stack.set_3dviewpoint(options.get_arg("--3dviewpoint", "1:1:1:2"));

  // Performance options
  if (options.has_flag("--threads"))
  {
    stack.set_threads(std::stoi(options.get_arg("--threads")));
  }

  if (options.has_flag("--batchsize"))
  {
    stack.set_batchsize(std::stoi(options.get_arg("--batchsize")));
  }

  stack.set_disable_opencl(options.has_flag("--no-opencl"));
  stack.set_wait_images(std::stof(options.get_arg("--wait-images", "0.0")));

  // Information options (some are handled at beginning of this function)
  stack.set_verbose(options.has_flag("--verbose"));

  // Check for any unhandled options
  std::vector<std::string> unparsed = options.get_unparsed();
  if (unparsed.size())
  {
    std::cerr << "Warning: unknown options: ";
    for (std::string arg: unparsed)
    {
      std::cerr << arg << " ";
    }
    std::cerr << std::endl;
  }


  if (!stack.run())
  {
    std::printf("\nError exit due to failed steps\n");
    return 1;
  }

  std::printf("\rSaved to %-40s\n", stack.get_output().c_str());

  if (stack.get_depthmap() != "")
  {
    std::printf("\rSaved depthmap to %s\n", stack.get_depthmap().c_str());
  }

  if (stack.get_3dview() != "")
  {
    std::printf("\rSaved 3D preview to %s\n", stack.get_3dview().c_str());
  }

  return 0;
}
