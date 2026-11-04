function grantAllPotentials() {
	let changesStarted = false;
	let actor = null;
	let desiredIds = null;
	let ownedAtStart = null;
	const compatible = () => {
		throw new Error("The current game does not expose a compatible potential API.");
	};
	const validId = value => Number.isInteger(value) && value > 0 && value <= 0xffffffff;
	const signature = potentials => {
		if (!Array.isArray(potentials) || potentials.length > 4096 ||
			potentials.some(potential => !potential || !validId(potential.id))) {
			compatible();
		}
		return potentials.map(potential => potential.id).sort((a, b) => a - b).join(",");
	};

	try {
		let load = typeof require === "function" ? require : null;
		if (!load) {
			const runtime = typeof puerts !== "undefined" ? puerts : typeof puer !== "undefined" ? puer : null;
			if (typeof runtime?.module?.createRequire !== "function") compatible();
			load = runtime.module.createRequire("");
		}
		if (typeof load !== "function") compatible();
		const cs = load("csharp", true);
		const battle = load("GamePlay/Battle/BattleJs", true).default?.Instance;
		if (typeof cs.PvpDefine?.IsOnlineMode !== "function") compatible();
		if (cs.PvpDefine.IsOnlineMode() || battle?.Battle?.IsLocalCoopMode) {
			throw new Error("Only single-player runs support granting all potentials.");
		}
		const kind = battle?.BattleClassName;
		const player = battle?.Battle?.PlayerSelf;
		if (!player || (kind !== "BattleBreed" && kind !== "BattleExplore")) {
			throw new Error("Enter a single-player run before granting potentials.");
		}
		if (!player.Attr || player.Attr.IsHPZero) {
			throw new Error("Wait until the current character is alive and ready.");
		}
		const model = kind === "BattleBreed"
			? load("Server/Model/BreedDungeon", true).ModelBreedDungeon?._instance
			: load("Server/Model/ExploreDungeon", true).ModelExploreDungeon?._instance;
		const data = model?.originData;
		const rawActor = kind === "BattleBreed" ? data?.actorInfo?.fesActor : data?.fesActor;
		if (!data || !rawActor ||
			(kind === "BattleBreed" ? data.finished || data.baseInfo?.actorId !== rawActor.id
				: data.finalBossFinished || !(data.mode > 0))) {
			throw new Error("Enter a single-player run before granting potentials.");
		}
		if (typeof battle.GetFesActor !== "function") compatible();
		const clientActor = battle.GetFesActor();
		if (!validId(rawActor.id) || !validId(rawActor.uid) ||
			player.BaseID !== rawActor.id || clientActor?.id !== rawActor.id ||
			clientActor?.uid !== rawActor.uid || signature(rawActor.potentials) !== signature(clientActor.potentials)) {
			throw new Error("The active character does not match the authoritative run data.");
		}

		const catalog = load("Gen/Xlsx", true).Xlsx?.BaseActorPotentialConf?.All;
		const utils = load("Server/Module/Actor", true).FesActorUtils;
		const potentialType = load("Gen/pbdef", true).STPotential;
		const utilities = load("Core/Util", true).U;
		if (!Array.isArray(catalog) || typeof utils?.AddPotential !== "function" ||
			typeof utils?.CalcFesActorScoreNoAttrModify !== "function" ||
			typeof potentialType?.create !== "function" || typeof utilities?.DeepCopy !== "function" ||
			typeof model.dirtyChecker?.forceSetDirty !== "function" ||
			!("_potentialRandomGenerator" in model) || typeof battle.AddPlayerTriggersFromFesActor !== "function" ||
			typeof player.SkillMgr?.MarkSkillChainNeedRebuild !== "function" ||
			typeof battle.UIMain?.RefreshBlessStatusUI !== "function") {
			compatible();
		}
		const rows = catalog.filter(row => row?.actorId === rawActor.id && !row.invalid && !row.isDamageSkill);
		desiredIds = new Set(rows.map(row => row.id));
		if (!rows.length) {
			throw new Error("No valid potentials were found for the current character.");
		}
		if (rows.length > 4096 || desiredIds.size !== rows.length || rows.some(row => !validId(row.id))) {
			compatible();
		}
		ownedAtStart = new Set(rawActor.potentials.map(potential => potential.id));
		const pending = rows.filter(row => !ownedAtStart.has(row.id))
			.sort((a, b) => a.id - b.id).map(row => {
				const potential = potentialType.create({id: row.id});
				if (potential?.id !== row.id) compatible();
				return potential;
			});
		if (!pending.length) {
			return JSON.stringify({ok: true, actorId: rawActor.id, added: 0, total: rows.length});
		}
		const projected = utilities.DeepCopy(rawActor);
		if (!projected || signature(projected.potentials) !== signature(rawActor.potentials)) compatible();
		projected.potentials.push(...pending);
		utils.CalcFesActorScoreNoAttrModify(projected);
		if (!Number.isFinite(projected.score) || projected.score < 0) compatible();
		actor = model.FesActor;
		if (actor?.id !== rawActor.id || actor?.uid !== rawActor.uid ||
			signature(actor.potentials) !== signature(rawActor.potentials)) {
			throw new Error("The active character does not match the authoritative run data.");
		}
		changesStarted = true;
		for (const potential of pending) {
			utils.AddPotential(actor, potential);
		}
		if (!rows.every(row => actor.potentials.some(potential => potential.id === row.id))) {
			throw new Error("The game did not retain every requested potential.");
		}
		actor.score = projected.score;
		model._potentialRandomGenerator = null;
		model.dirtyChecker.forceSetDirty();
		clientActor.potentials = utilities.DeepCopy(actor.potentials);
		clientActor.score = actor.score;
		if (signature(clientActor.potentials) !== signature(actor.potentials)) {
			throw new Error("The game did not retain every requested potential.");
		}
		battle.AddPlayerTriggersFromFesActor(player, clientActor);
		player.SkillMgr.MarkSkillChainNeedRebuild();
		battle.UIMain.RefreshBlessStatusUI(true);
		return JSON.stringify({ok: true, actorId: actor.id, added: pending.length, total: rows.length});
	} catch (error) {
		const added = Array.isArray(actor?.potentials) && desiredIds && ownedAtStart
			? new Set(actor.potentials.filter(potential => potential && desiredIds.has(potential.id) &&
				!ownedAtStart.has(potential.id)).map(potential => potential.id)).size : 0;
		const reason = error instanceof Error ? error.message : String(error);
		return JSON.stringify({
			ok: false,
			added,
			error: changesStarted
				? "Potential granting failed after changes started; some changes may already have applied: " + reason
				: reason
		});
	}
}
