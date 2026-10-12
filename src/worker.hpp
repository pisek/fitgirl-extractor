#ifndef FITGIRL_WORKER_HPP
#define FITGIRL_WORKER_HPP

namespace fg {

// Entry point for "fitgirl-extractor.exe --worker ..."; never returns normally.
int worker_main(int argc, char * argv[]);

} // namespace fg

#endif // FITGIRL_WORKER_HPP
