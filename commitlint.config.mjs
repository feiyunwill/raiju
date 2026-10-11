export default {
	extends: ["@commitlint/config-conventional"],
	rules: {
		"scope-enum": [
			1,
			"always",
			[
				"payload",
				"engine",
				"processor",
				"gateway",
				"ui",
				"companion",
				"extension",
				"docs",
				"ci",
				"deps",
				"release",
			],
		],
	},
};
