#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <unordered_set>
#include <osmpbf/parsehelpers.h>
#include <osmpbf/pbistream.h>
#include <osmpbf/osmfilein.h>
#include <osmpbf/inode.h>

struct Config final {
public:
	std::vector<std::string> fileNames;
	std::vector<uint8_t> zoomLevels;
public:
	Config() {}
	~Config() {}
	int parse(int argc, char ** argv);
	void help(std::ostream &);
};


struct LatDeg {
	LatDeg(double v) : v(v) {}
	double v;
};

struct LatRad {
	double v;
	LatRad(double v) : v(v) {}
	LatRad(LatDeg const & v) : v(v.v/180.0*M_PI) {}
};

struct LonDeg {
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

struct State {
	std::unordered_set<Tile> tiles;
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
		else if ("-h" == token || "--help" == token) {
			return -1;
		}
		else {
			return -1;
		}
	}
	return 0;
}

void Config::help(std::ostream & out) {
	out << "prg -f filenames  -z zoomlevels";
}

int main(int argc, char ** argv) {
	Config cfg;
	State state;
	if (cfg.parse(argc, argv) < 0) {
		cfg.help(std::cout);
		std::cout << std::endl;
		return -1;
	}
	
	osmpbf::PbiStream pbi(cfg.fileNames);
	for(auto z: cfg.zoomLevels) {
		osmpbf::parseFile(pbi, [&](osmpbf::PrimitiveBlockInputAdaptor & pbi){
			for(osmpbf::INodeStream node(pbi.getNodeStream()); !node.isNull(); node.next()) {
				state.tiles.emplace(z, LatDeg(node.latd()), LonDeg(node.lond()));
			}
		});
		pbi.reset();
	}
	for(Tile const & t : state.tiles) {
		std::cout << t.d.x << ' ' << t.d.y << ' ' << t.d.z << '\n';
	}
	return 0;
}
