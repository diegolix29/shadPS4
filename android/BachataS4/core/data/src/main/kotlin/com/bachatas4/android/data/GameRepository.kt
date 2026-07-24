package com.bachatas4.android.data

import android.content.Context
import com.bachatas4.android.database.GameDao
import com.bachatas4.android.database.GameEntity
import com.bachatas4.android.model.Game
import javax.inject.Inject
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

class GameRepository @Inject constructor(
    private val gameDao: GameDao,
    @ApplicationContext private val context: Context,
) {
    fun observeGames(): Flow<List<Game>> =
        gameDao.observeAll().map { games -> games.map { it.toModel() } }

    suspend fun getGame(id: String): Game? = gameDao.getById(id)?.toModel()

    suspend fun addImportedGame(
        result: ContentImportResult,
        sourceUri: String,
        importedAtMs: Long,
    ) {
        gameDao.insert(
            GameEntity(
                id = result.game.id,
                title = result.game.title,
                relativePath = result.game.relativePath,
                sourceUri = sourceUri,
                importedAtMs = importedAtMs,
                subtitle = result.game.subtitle,
                detail = result.game.detail,
                lastLaunchedAtMs = 0L,
            ),
        )
    }

    /**
     * Scan the app-owned games directory for folders that are not in the database,
     * and automatically re-register them using their ParamSfo metadata.
     * Also drops leftover `.import-*` staging dirs when no import is active.
     */
    suspend fun syncOrphanedFolders() {
        val gamesRoot = context.filesDir.resolve("games")
        if (!gamesRoot.isDirectory) return

        val dbGames = gameDao.getAll().map { it.id }.toSet()
        val folders = gamesRoot.listFiles()?.filter { it.isDirectory } ?: return

        // Concurrent/crashed imports can leave staging trees; free space when idle.
        if (!ImportManager.isBusy()) {
            folders
                .filter { it.name.startsWith(".import-") }
                .forEach { staging -> runCatching { staging.deleteRecursively() } }
        }

        folders.forEach { folder ->
            val id = folder.name
            if (id.startsWith(".")) return@forEach
            if (id !in dbGames) {
                val sfoFile = GameIconPaths.paramSfo(context.filesDir, "games/$id")
                val sfo = if (sfoFile.isFile) {
                    runCatching { ParamSfoReader.parse(sfoFile.readBytes()) }.getOrNull()
                } else null

                val resolved = GameMetadataResolver.resolve(folderName = id, sfo = sfo)
                gameDao.insert(
                    GameEntity(
                        id = resolved.id,
                        title = resolved.title,
                        relativePath = "games/$id",
                        sourceUri = "",
                        importedAtMs = System.currentTimeMillis(),
                        subtitle = resolved.subtitle,
                        detail = resolved.detail,
                        lastLaunchedAtMs = 0L,
                    ),
                )
            }
        }
    }

    /**
     * Re-read TITLE from on-disk param.sfo for games that were imported under folder names.
     * Does not rename game ids or directories.
     */
    suspend fun backfillTitlesFromSfo() {
        val entities = gameDao.getAll()
        val byId = entities.associateBy { it.id }
        val updates = titlesToUpdate(
            games = entities.map { it.id to it.title },
            sfoTitleFor = { id ->
                val entity = byId[id] ?: return@titlesToUpdate null
                val file = GameIconPaths.paramSfo(context.filesDir, entity.relativePath)
                if (!file.isFile) return@titlesToUpdate null
                runCatching { ParamSfoReader.parse(file.readBytes()).title }.getOrNull()
            },
        )
        updates.forEach { (id, title) -> gameDao.updateTitle(id, title) }
    }

    suspend fun updateLastLaunched(id: String) {
        gameDao.updateLastLaunched(id, System.currentTimeMillis())
    }

    suspend fun deleteGame(id: String): Boolean {
        val game = gameDao.getById(id) ?: return false
        val gamesRoot = context.filesDir.resolve("games").canonicalFile
        val ownedPath = context.filesDir.resolve(game.relativePath).canonicalFile
        require(ownedPath.toPath().startsWith(gamesRoot.toPath())) { "Game path escapes app-owned storage" }
        if (ownedPath.exists() && !ownedPath.deleteRecursively()) return false
        return gameDao.deleteById(id) > 0
    }
}

private fun GameEntity.toModel(): Game =
    Game(
        id = id,
        title = title,
        relativePath = relativePath,
        subtitle = subtitle,
        detail = detail,
        lastLaunchedAtMs = lastLaunchedAtMs,
    )
