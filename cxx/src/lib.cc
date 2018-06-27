#include "lib.h"

// Linux only
#include <sys/poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "src/const.h"

namespace gstat {

std::ostream & operator<<(std::ostream& os, const GBranch &info) {
    os  << info.branch << " "
        << info.upstream << " "
        << std::to_string(info.local) << " ";

    return os;
}

std::ostream & operator<<(std::ostream& os, const GRemote &info) {
    os  << std::to_string(info.ahead) << " "
        << std::to_string(info.behind) << " ";

    return os;
}

std::ostream & operator<<(std::ostream& os, const GStats &info) {
    os  << std::to_string(info.staged) << " "
        << std::to_string(info.conflicts) << " "
        << std::to_string(info.changed) << " "
        << std::to_string(info.untracked) << " ";

    return os;
}

/**
 * This determines if we ware inside a worktree or not.
 * If not, the git root and tree directory are the same.
 * If so, the git_root is a file pointing to the tree_d in the source
 * repository on this filesystem. Read it, and find root from going up.
 *
 * Returns: std::string - The path to the tree directory.
 */
std::string GPaths::set_tree_d() {
    this->tree_d = this->git_root;

    if (file_is_dir(this->tree_d)) {
        return this->tree_d;
    }

    // File of format:
    //gitdir: /tmp/g/.git/worktrees/wg
    std::ifstream fin(this->tree_d.c_str());
    if (!fin.good()) {
        throw std::runtime_error("Could not open wotkree file: " + this->tree_d);
    }
    fin >> this->tree_d >> this->tree_d;
    fin.close();

    this->git_root = this->tree_d;
    while (basename(this->git_root) != ".git") {
        this->git_root = dirname(this->git_root);
    }

    return this->tree_d;
}

std::string GPaths::head() {
    return join(this->tree_d, std::string("HEAD"));
}

std::string GPaths::merge() {
    return join(this->tree_d, std::string("MERGE_HEAD"));
}

std::string GPaths::rebase() {
    return join(this->tree_d, std::string("rebase-apply"));
}

std::string GPaths::stash() {
    return join(join(join(this->git_root, std::string("logs")),
                std::string("refs")), std::string("stash"));
}

/**
 * Does the current STDIN file have __ANY__ input.
 *
 * Returns: Bool
 */
bool stdin_has_input() {
    struct pollfd fds;
    uint_fast8_t ret;
    fds.fd = 0;
    fds.events = POLLIN;

    ret = poll(&fds, 1, 0);

    return ret == 1;
}

/**
 * Run a command on the system in the current directory.
 *
 * Return: std:string of output captured
 *
 * Raises: runtime_error : Popen failed, returned NULL.
 */
std::string run_cmd(const char *cmd) {
    FILE *pfile = popen(cmd, "r");
    if (pfile == nullptr) {
        throw std::runtime_error("Could not execute cmd: " + std::string(cmd));
    }

    std::array<char, 100> buffer;
    std::string text;
    while (fgets(buffer.data(), 100, pfile) != nullptr) {
        text.append(buffer.data());
    }
    pclose(pfile);

    return text;
}

/**
 * Get the current working directory.
 *
 * Returns: A string of the path.
 *
 * Raises: runtime_error: Failed to get CWD.
 */
std::string get_cwd() {
    std::array<char, PATH_MAX> buffer;

    if ((getcwd(buffer.data(), PATH_MAX)) == nullptr) {
        throw std::runtime_error("Unable to get CWD");
    }

    return std::string(buffer.data());
}

/**
 * Move upward from the current directory until a directory
 * with `.git` is found.
 *
 * Returns: std::string - The path with git.
 */
std::string find_git_root() {
    std::string cwd = get_cwd();
    std::string git_d = ".git";
    std::string git_path = join(cwd, git_d);

    while (cwd != "/") {
        if (file_exists(git_path)) {
            return git_path;
        }

        cwd = dirname(cwd);
        git_path = join(cwd, git_d);
    }

    throw std::runtime_error("Could not find a git directory!");
}

/**
 * Parse the branch of a string line.
 *
 * Returns: GBranch structure.
 */
GBranch parse_branch(const std::string &branch_line, const std::string &head_file) {
    std::string temp = branch_line.substr(3);
    GBranch result;
    result.local = 1;

    std::size_t found = temp.rfind(" [");
    if (found != std::string::npos) {
        temp = temp.substr(0, found);
    }

    found = temp.find("...");
    if (found != std::string::npos) {
        result.branch = temp.substr(0, found);
        result.upstream = temp.substr(found + 3);
        result.local = 0;
    } else if (temp.find("(no branch)") != std::string::npos) {
        result.local = 0;
        std::ifstream fin(head_file.c_str());
        if (fin.good()) {
            fin >> result.branch;
        } else {
            throw std::runtime_error("Failed to get hash!");
        }
        fin.close();
    } else if (temp.find("Initial commit") != std::string::npos ||
               temp.find("No commits yet") != std::string::npos) {
        result.branch = temp.substr(temp.rfind(" ") + 1);
    } else {
        result.branch = temp;
    }

    return result;
}

/*
 * Parse the remote tracking portion of git status.
 *
 * Returns: GRemote structure.
 */
GRemote parse_remote(const std::string &branch_line) {
    GRemote remote;
    std::string temp = branch_line;

    std::size_t found = branch_line.find(" [");
    // Check for remote tracking (example: [ahead 2])
    if (found == std::string::npos ||
        branch_line.at(branch_line.length() - 1) != ']') {
        return remote;
    }

    // Only the remote tracking section remains
    temp = temp.substr(found + 2, temp.length() -1);

    if (temp.length() != 0 && temp.find("ahead") != std::string::npos) {
        temp = temp.replace(temp.begin(), temp.begin() + 6, "");
        std::string part;
        while (temp.length() != 0) {
            part += temp.at(0);
            temp = temp.substr(1);
            if (temp.length() == 0 || std::isdigit(temp.at(0)) == 0) {
                break;
            }
        }

        remote.ahead = std::stoi(part);
    }

    if (temp.length() != 0 && temp.at(0) == ',') {
        temp = temp.substr(1);
    }

    if (temp.length() != 0 && temp.find("behind") != std::string::npos) {
        temp = temp.replace(temp.begin(), temp.begin() + 7, "");
        remote.behind = std::stoi(temp);
    }

    return remote;
}

/**
 * Parses the status information from porcelain output.
 */
GStats parse_stats(const std::vector<std::string> &lines) {
    GStats stats;

    for (std::vector<std::string>::const_iterator i = lines.begin(); i != lines.end(); ++i) {
        if (i->at(0) == '?') {
            stats.untracked++;
            continue;
        }

        switch (hash_two_places(*i)) {
        case HASH_CASE_AA:
        case HASH_CASE_AU:
        case HASH_CASE_DD:
        case HASH_CASE_DU:
        case HASH_CASE_UA:
        case HASH_CASE_UD:
        case HASH_CASE_UU:
            stats.conflicts++;
            continue;
        }

        switch (i->at(0)) {
        case 'A':
        case 'C':
        case 'D':
        case 'M':
        case 'R':
            stats.staged++;
        }

        switch (i->at(1)) {
        case 'C':
        case 'D':
        case 'M':
        case 'R':
            stats.changed++;
        }
    }

    return stats;
}

/**
 * Returns: std:string - The # of stashes on repo
 */
std::string stash_count(const std::string &stash_file) {
    std::uint_fast32_t count = 0;

    std::ifstream fin(stash_file.c_str());
    while (fin.good()) {
        std::string buffer;
        std::getline(fin, buffer);
        if (buffer != "") {
            ++count;
        }
    }
    fin.close();

    return std::to_string(count);
}

/**
 * Returns:
 *  - "0": No active rebase
 *  - "1/4": Rebase in progress, commit 1 of 4
 */
std::string rebase_progress(const std::string &rebase_d) {
    std::string result = "0";
    std::string temp;

    std::ifstream next(join(rebase_d, std::string("next")).c_str());
    std::ifstream last(join(rebase_d, std::string("last")).c_str());
    if (next.good() && last.good()) {
        result.clear();
        last >> temp;
        next >> result;
        result += "/" + temp;
    }
    last.close();
    next.close();

    return result;
}

/**
 * Take input and produce the required output per specification.
 */
std::string current_gitstatus(const std::vector<std::string> &lines) {
    GPaths path(gstat::find_git_root());
    GBranch info = parse_branch(lines[0], path.head());
    GRemote remote = parse_remote(lines[0]);
    GStats stats = parse_stats(lines);
    std::string stashes = stash_count(path.stash());
    std::string merge = std::to_string(
                        static_cast<uint_fast8_t>(file_exists(path.merge())));
    std::string rebase = rebase_progress(path.rebase());

    std::ostringstream ss;
    ss << info.branch << " " << remote << stats << stashes << " "
        << std::to_string(info.local) << " " << info.upstream << " "
        << merge << " " << rebase;;

    return ss.str();
}

}  // namespace gstat