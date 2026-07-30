// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Core Video Header
//

// ==========
// Interfaces
// ==========

// Video Manager
class core_IVideo {
public:
	static core_IVideo* GetHandle(sys_IMain* sysHnd);
	static void FreeHandle(core_IVideo* hnd);

	// Returns true when the platform window could not be applied.
	virtual bool	Apply(bool shown = true) = 0;
	virtual void	Save() = 0;
};
