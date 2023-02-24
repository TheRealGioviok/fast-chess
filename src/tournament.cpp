#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "elo.h"
#include "pgn_builder.h"
#include "rand.h"
#include "tournament.h"

Tournament::Tournament(const CMD::GameManagerOptions &mc)
{
    loadConfig(mc);
}

void Tournament::loadConfig(const CMD::GameManagerOptions &mc)
{
    matchConfig = mc;

    if (matchConfig.opening.file != "")
    {
        std::ifstream openingFile;
        std::string line;
        openingFile.open(matchConfig.opening.file);

        while (std::getline(openingFile, line))
        {
            openingBook.emplace_back(line);
        }

        openingFile.close();
    }
}

std::string Tournament::fetchNextFen()
{
    if (openingBook.size() == 0)
    {
        return "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    }
    else if (matchConfig.opening.format == "pgn")
    {
        // todo: implementation
    }
    else if (matchConfig.opening.format == "epd")
    {
        if (matchConfig.opening.order == "random")
        {
            std::uniform_int_distribution<uint64_t> maxLines{startIndex % (openingBook.size() - 1),
                                                             openingBook.size() - 1};

            auto randLine = maxLines(Random::generator);
            assert(randLine >= 0 && randLine < openingBook.size());

            return openingBook[randLine];
        }
        else if (matchConfig.opening.order == "sequential")
        {
            assert(startIndex++ % (openingBook.size() - 1) >= 0 &&
                   startIndex++ % (openingBook.size() - 1) < openingBook.size());

            return openingBook[startIndex++ % (openingBook.size() - 1)];
        }
    }

    return "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
}

std::vector<std::string> Tournament::getPGNS() const
{
    return pgns;
}

Match Tournament::startMatch(UciEngine &engine1, UciEngine &engine2, int round, std::string openingFen)
{
    const int64_t timeoutThreshold = 0;
    bool timeout = false;

    std::vector<std::string> output;
    output.reserve(30);

    GameResult res;
    Match match;
    Move move;

    Board board;
    board.loadFen(openingFen);

    match.whiteEngine = board.sideToMove == WHITE ? engine1.getConfig() : engine2.getConfig();
    match.blackEngine = board.sideToMove != WHITE ? engine1.getConfig() : engine2.getConfig();

    engine1.sendUciNewGame();
    engine2.sendUciNewGame();

    match.date = saveTimeHeader ? getDateTime("%Y-%m-%d") : "";
    match.startTime = saveTimeHeader ? getDateTime() : "";
    match.board = board;

    std::string positionInput = "position startpos moves";
    std::string bestMove;

    auto timeLeft_1 = engine1.getConfig().tc;
    auto timeLeft_2 = engine2.getConfig().tc;

    auto start = std::chrono::high_resolution_clock::now();
    std::chrono::high_resolution_clock::time_point t0;
    std::chrono::high_resolution_clock::time_point t1;

    while (true)
    {
        // Check for game over
        res = board.isGameOver();
        if (res != GameResult::NONE)
        {
            break;
        }

        // Engine 1's turn
        // Write new position
        engine1.writeProcess(positionInput);
        engine1.writeProcess(engine1.buildGoInput(board.sideToMove, timeLeft_1));

        // Start measuring time
        t0 = std::chrono::high_resolution_clock::now();

        output = engine1.readProcess("bestmove", timeout, timeoutThreshold);

        t1 = std::chrono::high_resolution_clock::now();

        // Subtract measured time
        timeLeft_1.time -=
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() - timeLeft_1.increment;

        // Timeout!
        if (timeLeft_1.time < 0)
        {
            res = GameResult(~board.sideToMove);
            break;
        }

        // find bestmove and add it to the position string
        bestMove = findElement<std::string>(splitString(output.back(), ' '), "bestmove");
        positionInput += " " + bestMove;

        // play move on internal board and store it for later pgn creation
        move = convertUciToMove(bestMove);
        board.makeMove(move);
        match.moves.emplace_back(move);

        // Check for game over
        res = board.isGameOver();
        if (res != GameResult::NONE)
        {
            break;
        }

        // Engine 1's turn
        // Write new position
        engine2.writeProcess(positionInput);
        engine2.writeProcess(engine2.buildGoInput(board.sideToMove, timeLeft_2));

        // Start measuring time
        t0 = std::chrono::high_resolution_clock::now();

        output = engine2.readProcess("bestmove", timeout, timeoutThreshold);

        t1 = std::chrono::high_resolution_clock::now();

        // Subtract measured time
        timeLeft_2.time -=
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() - timeLeft_2.increment;

        // Timeout!
        if (timeLeft_2.time < 0)
        {
            res = GameResult(~board.sideToMove);
            break;
        }

        // find bestmove and add it to the position string
        bestMove = findElement<std::string>(splitString(output.back(), ' '), "bestmove");
        positionInput += " " + bestMove;

        // play move on internal board and store it for later pgn creation
        move = convertUciToMove(bestMove);
        board.makeMove(move);
        match.moves.emplace_back(move);
    }

    auto end = std::chrono::high_resolution_clock::now();

    match.round = round;
    match.result = res;
    match.endTime = saveTimeHeader ? getDateTime() : "";
    match.duration =
        saveTimeHeader ? formatDuration(std::chrono::duration_cast<std::chrono::seconds>(end - start)) : "";

    return match;
}

std::vector<Match> Tournament::runH2H(CMD::GameManagerOptions localMatchConfig,
                                      std::vector<EngineConfiguration> configs, int gameId, std::string fen)
{
    // Initialize variables
    std::vector<Match> matches;

    UciEngine engine1, engine2;
    engine1.loadConfig(configs[0]);
    engine2.loadConfig(configs[1]);

    engine1.startEngine();
    engine2.startEngine();

    // engine1 always starts first
    engine1.turn = Turn::FIRST;
    engine2.turn = Turn::SECOND;

    int rounds = localMatchConfig.repeat ? 2 : 1;

    for (int i = 0; i < rounds; i++)
    {
        Match match;
        if (engine1.turn == Turn::FIRST)
            match = startMatch(engine1, engine2, i, fen);
        else
            match = startMatch(engine2, engine1, i, fen);

        matches.emplace_back(match);

        std::string positiveEngine = engine1.turn == Turn::FIRST ? engine1.getConfig().name : engine2.getConfig().name;
        std::string negativeEngine = engine1.turn == Turn::FIRST ? engine2.getConfig().name : engine1.getConfig().name;

        // create a pgn from the played match
        PgnBuilder pgn(match, matchConfig);

        // use a stringstream to build the output to avoid data races with cout <<
        std::stringstream ss;
        ss << "Finished " << gameId + i << "/" << localMatchConfig.games * rounds << " " << positiveEngine << " vs "
           << negativeEngine << "\n";
        //    << pgn.getPGN() << "\n";

        std::cout << ss.str();

        engine1.turn = ~engine1.turn;
        engine2.turn = ~engine2.turn;
    }

    return matches;
}

void Tournament::startTournament(std::vector<EngineConfiguration> configs)
{
    pgns.clear();
    pool.resize(matchConfig.concurrency);

    std::vector<std::future<std::vector<Match>>> results;

    int rounds = matchConfig.repeat ? 2 : 1;

    for (int i = 1; i <= matchConfig.games * rounds; i += rounds)
    {
        results.emplace_back(
            pool.enqueue(std::bind(&Tournament::runH2H, this, matchConfig, configs, i, fetchNextFen())));
    }

    int wins = 0;
    int draws = 0;
    int losses = 0;

    int gameCount = 0;

    for (auto &&result : results)
    {
        auto res = result.get();

        for (auto match : res)
        {
            gameCount++;

            PgnBuilder pgn(match, matchConfig);
            pgns.emplace_back(pgn.getPGN());

            if (match.result == GameResult::WHITE_WIN)
            {
                if (match.whiteEngine.name == configs[0].name)
                    wins++;
                else
                    losses++;
            }
            else if (match.result == GameResult::BLACK_WIN)
            {
                if (match.blackEngine.name == configs[0].name)
                    wins++;
                else
                    losses++;
            }
            else if (match.result == GameResult::DRAW)
            {
                draws++;
            }
            else
            {
                std::cout << "Couldnt obtain Game Result" << std::endl;
            }

            Elo elo(wins, losses, draws);

            std::cout << "Score of " << configs[0].name << " vs " << configs[1].name << ": " << wins << " - " << losses
                      << " - " << draws << " Games: " << gameCount << "\n"
                      << "Elo difference: " << elo.getElo() << std::endl;
        }
    }
}

std::string Tournament::getDateTime(std::string format)
{
    // Get the current time in UTC
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    // Format the time as an ISO 8601 string
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t_now), format.c_str());
    return ss.str();
}

std::string Tournament::formatDuration(std::chrono::seconds duration)
{
    auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
    duration -= hours;
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
    duration -= minutes;
    auto seconds = duration;

    std::stringstream ss;
    ss << std::setfill('0') << std::setw(2) << hours.count() << ":" << std::setfill('0') << std::setw(2)
       << minutes.count() << ":" << std::setfill('0') << std::setw(2) << seconds.count();
    return ss.str();
}