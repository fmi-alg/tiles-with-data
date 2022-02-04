#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <unordered_set>
#include <unordered_map>
#include <osmpbf/parsehelpers.h>
#include <osmpbf/pbistream.h>
#include <osmpbf/osmfilein.h>
#include <osmpbf/inode.h>

#include <sys/stat.h>
#include <fcntl.h>

#if defined(__linux__)
#  if defined(__ANDROID__)
#    include <sys/endian.h>
#    ifndef le64toh
#      define le64toh(x) letoh64(x)
#    endif
#  else
#    include <endian.h>
#  endif
#elif defined(__FreeBSD__) || defined(__NetBSD__)
#  include <sys/endian.h>
#elif defined(__OpenBSD__)
#  include <sys/types.h>
#else
#  include <arpa/inet.h>
#endif

struct Config final {
public:
	std::vector<std::string> fileNames;
	std::vector<uint8_t> zoomLevels;
	uint32_t threadCount{1};
	bool binaryOutput{false};
	bool count{false};
public:
	Config() {}
	~Config() {}
	int parse(int argc, char ** argv);
	void help(std::ostream &);
};

struct LatDeg final {
	LatDeg(double v) : v(v) {}
	double v;
};

struct LatRad final {
	double v;
	LatRad(double v) : v(v) {}
	LatRad(LatDeg const & v) : v(v.v/180.0*M_PI) {}
};

struct LonDeg final {
	LonDeg(double v) : v(v) {}
	double v;
};

union Tile final {
	struct Data final {
		uint64_t z:6;
		uint64_t x:29;
		uint64_t y:29;
	} d;
	uint64_t raw;
	Tile(uint64_t raw) : raw(raw) {}
	Tile(uint8_t z, uint32_t x, uint32_t y) {
		d.z = z;
		d.x = x;
		d.y = y;
	}
	Tile(uint8_t z, LatRad latRad, LonDeg lonDeg) {
		d.z = z;
		uint32_t n =  uint32_t(1) << z;
		d.x = n * ((lonDeg.v + 180) / 360);
		d.y = n * (1 - (std::log(std::tan(latRad.v) + (1.0/std::cos(latRad.v))) / M_PI)) / 2;
	}
	bool operator==(Tile const & other) const { return raw == other.raw; }
};

namespace std {
	template<>
	struct hash<Tile> {
		std::hash<uint64_t> h;
		inline auto operator()(Tile const & t) const { return h(t.raw); }
	};
}

struct State final {
	std::mutex lock;
	std::unordered_set<Tile> tiles;
	
	//stats
	std::size_t dataSize;
	std::atomic<uint64_t> parsedDataSize{0};
};

struct Worker final {
	Config * cfg;
	State * state;
	std::unordered_set<Tile> tiles;
	void operator()(osmpbf::PrimitiveBlockInputAdaptor & pbi) {
		if (!pbi.nodesSize()) {
			return;
		}
		for(auto z : cfg->zoomLevels) {
			for(osmpbf::INodeStream node(pbi.getNodeStream()); !node.isNull(); node.next()) {
				tiles.emplace(z, LatDeg(node.latd()), LonDeg(node.lond()));
			}
		}
	}
	Worker(Config * cfg, State * state) : cfg(cfg), state(state) {}
	Worker(Worker const & other) : cfg(other.cfg), state(other.state) {}
	~Worker() {
		std::lock_guard<std::mutex> lck(state->lock);
		if (state->tiles.size()) {
			state->tiles.insert(tiles.begin(), tiles.end());
		}
		else {
			std::swap(state->tiles, tiles);
		}
	}
	
};

int Config::parse(int argc, char ** argv) {
	for(int i(1); i < argc;) {
		std::string token(argv[i]);
		if ("-f" == token) {
			for(++i; i < argc; ++i) {
				token = std::string(argv[i]);
				if (token.at(0) != '-') {
					fileNames.push_back(token);
				}
				else {
					break;
				}
			}
		}
		else if ("-z" == token) {
			for(++i; i < argc; ++i) {
				token = std::string(argv[i]);
				if (token.at(0) != '-') {
					auto zl = std::atoi(token.c_str());
					if (0 <= zl && zl <= 22) {
						zoomLevels.push_back(zl);
					}
					else {
						std::cerr << "Invalid zoomlevel: " << zl << std::endl;
					}
				}
				else {
					break;
				}
			}
		}
		else if (("-t" == token || "--threads" == token) && i+1 < argc) {
			threadCount = ::atoi(argv[i+1]);
			i += 2;
		}
		else if ("-b" == token || "--binary" == token) {
			binaryOutput = true;
			++i;
		}
		else if ("--count" == token) {
			count = true;
			++i;
		}
		else if ("-h" == token || "--help" == token) {
			return -1;
		}
		else {
			std::cerr << "Invalid option: " << token << std::endl;
			return -1;
		}
	}
	return 0;
}

void Config::help(std::ostream & out) {
	out <<
		"tiles-with-data -f filenames  -z zoomlevels [--binary] [--count]\n"
		"Binary format is uint64_t in little endian with\n"
		"uint64_t v = (uint64_t(t.d.z) << 58) | (uint64_t(t.d.y) << 29) | (uint64_t(t.d.x))\n"
		"List all tiles in zoom levels 10 to 14 with data using 8 threads:\n"
		"tiles-with-data -f planet.osm.ppbf -z 10 11 12 13 14 -t 8 > tiles.txt";
}

int main(int argc, char ** argv) {
	Config cfg;
	State state;
	if (cfg.parse(argc, argv) < 0 || !cfg.fileNames.size() || !cfg.zoomLevels.size()) {
		cfg.help(std::cout);
		std::cout << std::endl;
		return -1;
	}
	
	osmpbf::PbiStream pbi(cfg.fileNames);
	osmpbf::parseFileCPPThreads(pbi, Worker(&cfg, &state), cfg.threadCount, 1, true);
	if (cfg.count) {
		std::unordered_map<std::size_t, std::size_t> counts;
		for(Tile const & t : state.tiles) {
			counts[t.d.z] += 1;
		}
		for(auto const & [zoom, count] : counts) {
			std::cout << zoom << ": " << count << std::endl;
		}
	}
	else if (cfg.binaryOutput) {
		for(Tile const & t : state.tiles) {
			static_assert(sizeof(uint8_t) == 1 && sizeof(uint64_t) == 8);
			uint64_t d = (uint64_t(t.d.z) << 58) | (uint64_t(t.d.y) << 29) | (uint64_t(t.d.x));
			d = htole64(d);
			char tmp[8];
			::memcpy(tmp, &d, 8);
			std::cout.write(tmp, 8);
		}
	}
	else {
		for(Tile const & t : state.tiles) {
			std::cout << t.d.x << ' ' << t.d.y << ' ' << t.d.z << '\n';
		}
	}
	return 0;
}
