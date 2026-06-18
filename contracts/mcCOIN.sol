// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "@openzeppelin/contracts/token/ERC20/ERC20.sol";
import "@openzeppelin/contracts/access/Ownable2Step.sol";
import "@openzeppelin/contracts/utils/Pausable.sol";
import "@openzeppelin/contracts/utils/cryptography/MerkleProof.sol";

/// @title mcCOIN — wrapped musicchain coin on Base
///
/// @notice ERC-20 representation of musicchain's native coin. Designed
///   to scale to Spotify-level playback volume by separating two paths:
///
///   * **Deposit / claim:** the founder daemon publishes a daily merkle
///     root over every player's cumulative bridge-eligible balance.
///     Users `claim()` whenever they want with a merkle proof. One
///     `setRoot()` per day vs N per-play mints means the bridge gas
///     bill stops scaling with playback. Users pay their own claim
///     gas, which is why this works at >10⁹ plays/day where naive
///     auto-bridge would burn ~$240M/day on Base.
///
///   * **Withdraw / burn:** unchanged from the original draft. Users
///     burn mcCOIN with an embedded musicchain payout address; the
///     daemon credits native mc on the other side. Burn frequency is
///     dictated by users actually exiting to fiat — orders of
///     magnitude lower than deposit volume.
///
/// @dev Trust model: the bridge owner (= Gnosis Safe) controls
///   `setRoot()` so they could publish a forged tree that mints
///   unbacked mcCOIN. Audits verify (a) every root is reconstructible
///   from the public merkle data hosted at `currentDataURI`, and
///   (b) the sum of all leaves matches the musicchain side's view of
///   bridgeable native-mc balance. Discrepancies trigger
///   `pause()`. The cap is a final backstop.
contract McCoin is ERC20, Ownable2Step, Pausable {
    // ---- Events -----------------------------------------------------

    /// @notice Emitted whenever a new merkle root is published. The
    ///   `dataURI` points at the raw leaf data (IPFS / HTTPS) so any
    ///   independent observer can rebuild the tree and verify the
    ///   founder's accounting against the chain.
    event RootUpdated(uint256 indexed epoch,
                      bytes32 root,
                      string  dataURI);

    /// @notice Emitted on each successful claim. `newAmount` is the
    ///   delta minted to the user (= cumulative - previously-claimed).
    event Claimed(address indexed user,
                  uint256 indexed epoch,
                  uint256 newAmount);

    /// @notice Emitted on user-initiated withdraw. `mcAddress` is the
    ///   20-byte destination on musicchain.
    event BridgeBurn(address indexed from,
                     uint256 amount,
                     bytes20 mcAddress);

    // ---- Storage ----------------------------------------------------

    /// @notice Hard supply ceiling, scaled from musicchain's 8-decimal
    ///   SUPPLY_CAP up to Base's 18-decimal convention.
    uint256 public immutable cap;

    /// @notice The currently-active root. Each new `setRoot()` replaces
    ///   the previous root entirely; the latest root carries
    ///   cumulative-to-date balances, so older roots become a subset
    ///   and never need to be re-verified independently.
    bytes32 public currentRoot;

    /// @notice Monotonic epoch counter incremented by each setRoot
    ///   call. Indexed in `Claimed` events so a user can prove "I
    ///   claimed under root N" without storing the root itself.
    uint256 public currentEpoch;

    /// @notice IPFS / HTTPS URI of the raw leaf data for `currentRoot`.
    ///   Used by independent auditors and by the player UI to fetch
    ///   the user's own proof.
    string public currentDataURI;

    /// @notice Cumulative mcCOIN minted per user across every claim
    ///   they've ever made. Stored so a user can claim against an
    ///   updated cumulative balance by paying only for the delta.
    mapping(address => uint256) public claimed;

    // ---- Construction -----------------------------------------------

    constructor(uint256 cap_, address initialOwner)
        ERC20("mcCOIN", "MC")
        Ownable(initialOwner)
    {
        require(cap_ > 0,                  "mcCOIN: cap = 0");
        require(initialOwner != address(0), "mcCOIN: owner = 0");
        cap = cap_;
    }

    // ---- Owner publishes the daily root ----------------------------

    /// @notice Publish a new merkle root. The leaves must be of the
    ///   form `keccak256(bytes.concat(keccak256(abi.encode(user,
    ///   cumulativeAmount))))` — the OpenZeppelin standard double-hash
    ///   format used by `MerkleProof.verify`.
    /// @param root The new merkle root committing to every bridge-
    ///   eligible musicchain user's cumulative bridgeable balance.
    /// @param dataURI Public URI (ipfs://, https://) of the raw leaf
    ///   data so audits can rebuild and verify the root off-chain.
    function setRoot(bytes32 root, string calldata dataURI)
        external onlyOwner whenNotPaused
    {
        require(root != bytes32(0),   "mcCOIN: root = 0");
        require(bytes(dataURI).length > 0, "mcCOIN: dataURI empty");
        currentEpoch  += 1;
        currentRoot    = root;
        currentDataURI = dataURI;
        emit RootUpdated(currentEpoch, root, dataURI);
    }

    // ---- User claims their delta -----------------------------------

    /// @notice Claim the difference between your cumulative
    ///   bridge-eligible balance (as committed in the current root)
    ///   and what you've already claimed. Caller pays gas. Idempotent
    ///   relative to the current root — calling twice with the same
    ///   proof reverts on `nothing new`.
    /// @param cumulativeAmount The leaf-side cumulative value as
    ///   published in the merkle data for `msg.sender`.
    /// @param proof The merkle proof from `currentDataURI`.
    function claim(uint256 cumulativeAmount, bytes32[] calldata proof)
        external whenNotPaused
    {
        require(currentRoot != bytes32(0), "mcCOIN: no root yet");

        // OZ standard double-hashed leaf to prevent second-preimage
        // attacks on internal nodes.
        bytes32 leaf = keccak256(
            bytes.concat(keccak256(abi.encode(msg.sender, cumulativeAmount)))
        );
        require(MerkleProof.verify(proof, currentRoot, leaf),
                "mcCOIN: bad proof");

        uint256 already = claimed[msg.sender];
        require(cumulativeAmount > already, "mcCOIN: nothing new");
        uint256 newAmount = cumulativeAmount - already;

        // Cap check happens against totalSupply so a forged root that
        // would mint above cap is rejected one claim at a time. (The
        // tree-sum audit catches the same issue earlier.)
        require(totalSupply() + newAmount <= cap, "mcCOIN: cap exceeded");

        claimed[msg.sender] = cumulativeAmount;
        _mint(msg.sender, newAmount);
        emit Claimed(msg.sender, currentEpoch, newAmount);
    }

    /// @notice Return the new amount that would be minted if `user`
    ///   claimed `cumulativeAmount` right now. View-only helper for
    ///   player UIs to render "Bridge X mc → claim Y mcCOIN".
    function previewClaim(address user, uint256 cumulativeAmount)
        external view returns (uint256)
    {
        if (cumulativeAmount <= claimed[user]) return 0;
        return cumulativeAmount - claimed[user];
    }

    // ---- Withdraw side (unchanged from v0) -------------------------

    /// @notice Burn mcCOIN to initiate a withdraw back to musicchain.
    function burn(uint256 amount, bytes20 mcAddress)
        external whenNotPaused
    {
        require(amount > 0,              "mcCOIN: amount = 0");
        require(mcAddress != bytes20(0), "mcCOIN: mcAddress = 0");
        _burn(msg.sender, amount);
        emit BridgeBurn(msg.sender, amount, mcAddress);
    }

    // ---- Owner controls --------------------------------------------

    function pause()   external onlyOwner { _pause(); }
    function unpause() external onlyOwner { _unpause(); }
}
