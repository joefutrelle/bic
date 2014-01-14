#include <iostream>
#include <fstream>
#include <string>
#include <opencv2/opencv.hpp>
#include "prototype.hpp"
#include "interpolation.hpp"
#include "demosaic.hpp"

using namespace std;

void demoBinning() { // FIXME delete
  interp::LinearBinning<int> bins = interp::LinearBinning<int>(0,100,7);
  typedef vector<pair<int, double> > interp_t;
  interp_t result = bins.interpolate(31);
  interp_t::iterator it = result.begin();
  for(; it != result.end(); ++it) {
    pair<int,double> p = *it;
    cout << p.first << ": " << p.second << endl;
  }
}

int main(int argc, char **argv) {
  //learn_prototype();
  using namespace cv;
  Mat cfa = imread(argv[1],CV_LOAD_IMAGE_ANYDEPTH);
  cfa = cfa_smooth(cfa,31);
  imwrite("cfa_smooth.tiff",cfa);
  imwrite("rgb_smooth.tiff",demosaic(cfa,"rggb"));
}
