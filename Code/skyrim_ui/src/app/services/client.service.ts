import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { AsyncSubject, BehaviorSubject, ReplaySubject, Subject } from 'rxjs';
import { environment } from '../../environments/environment';
import { Debug } from '../models/debug';
import { PartyInfo } from '../models/party-info';
import { Player } from '../models/player';
import { ChatService, MessageTypes } from './chat.service';
import { ErrorEvents, ErrorService } from './error.service';
import { LoadingService } from './loading.service';

export interface CharacterCheckpoint {
  required: boolean; phase: string; name: string; revision: string; detail: string;
}
export interface CharacterSlot { label: string; key: string; name?: string; race?: string; level?: number; location?: string; playtime?: string; portrait?: string; }
const emptyCheckpoint: CharacterCheckpoint = { required: false, phase: 'disabled', name: '', revision: '0', detail: '' };

export interface TravelInvitation {
  id: number;
  expiresAt: number;
  count: number;
  canCancel: boolean;
  accepted: boolean;
  leader: string;
  destination: string;
}

export interface SharedDialoguePanel {
  actorId: number;
  owner: string;
  canChoose: boolean;
  canVote: boolean;
  canPass: boolean;
  isPrivate: boolean;
  revision: number;
  npc: string;
  options: string[];
  votes: number[];
  voterIds: number[];
  voterOptions: number[];
  selected: number;
  remote: boolean;
  listening: boolean;
  subtitle: string;
}

/** Client game service. */
@Injectable({
  providedIn: 'root',
})
export class ClientService implements OnDestroy {
  /** Initialization done. */
  public readonly travelInvitation = new BehaviorSubject<TravelInvitation | undefined>(undefined);
  public sharedDialogueChange = new BehaviorSubject<SharedDialoguePanel[]>([]);
  public readonly dialogueFocus = new BehaviorSubject<{ open: boolean; actorId: number }>({ open: false, actorId: 0 });
  public readonly characterPortraits = new BehaviorSubject<ReadonlyMap<number, string>>(new Map());

  public initDone = new AsyncSubject<undefined>();

  /** Activation state change. */
  public activationStateChange = new BehaviorSubject(false);

  /** InGame state change. */
  public inGameStateChange = new BehaviorSubject(!environment.game);

  /** Opening/close menu change. */
  public openingMenuChange = new BehaviorSubject(false);

  /** Connection state change. */
  public connectionStateChange = new BehaviorSubject(false);
  public readonly openSettings = new Subject<void>();
  public readonly mainMenuChange = new BehaviorSubject(false);
  public readonly lobbyStateChange = new BehaviorSubject(false);
  public readonly checkpointChange = new BehaviorSubject<CharacterCheckpoint>(emptyCheckpoint);
  public readonly characterSlotsChange = new BehaviorSubject<CharacterSlot[]>([]);

  public isConnectionInProgressChange = new BehaviorSubject(false);

  /** Player name change. */
  public nameChange = new BehaviorSubject(environment.game ? '' : 'test');

  /** Client version setting. */
  public versionSet = new BehaviorSubject(environment.game ? '' : 'browser');

  /** Debug state change. */
  public debugStateChange = new Subject<boolean>();

  /** Debug data change. */
  public debugDataChange = new BehaviorSubject(new Debug());

  /** Connect player to server change. */
  public playerConnectedChange = new Subject<Player>();

  /** Connect party info change. */
  public partyInfoChange = new Subject<PartyInfo>();

  /** Connect party info change. */
  public partyLeftChange = new Subject<void>();

  /** Connect party invite received. */
  public partyInviteReceivedChange = new Subject<number>();

  /** Local quest started or updated while not in a party. */
  public questUpdatedChange = new Subject<void>();

  /** Disconnect player to server change. */
  public playerDisconnectedChange = new Subject<Player>();

  /** Player health change. */
  public healthChange = new Subject<Player>();

  /** Player level change. */
  public levelChange = new Subject<Player>();

  /** Player cell change. */
  public cellChange = new Subject<Player>();

  /** Player isLoaded change. */
  public isLoadedChange = new Subject<Player>();

  public protocolMismatchChange = new BehaviorSubject(false);

  /** Receive error from core */
  public triggerError = new ReplaySubject<ErrorEvents>(1);

  /** Used purely for debugging. */
  public debugChange = new Subject<void>();

  // The below emitters are used in the mocking service

  /** Used for when a party leader changed. */
  public partyLaunchedChange = new Subject<void>();

  /** Used for when a party invite is sent. */
  public partyInviteChange = new Subject<number>();

  /** Used for when a party invite was accepted by the local player. */
  public partyJoinedChange = new Subject<number>();

  /** Used for when a party member was kicked. */
  public memberKickedChange = new Subject<number>();

  /** Used for when a party leader changed. */
  public partyLeaderChange = new Subject<number>();

  public localPlayerId = undefined;

  private _host: string;

  private _port: number;

  private _password: string;

  private _remainingReconnectionAttempt = environment.nbReconnectionAttempts;

  /**
   * Instantiate.
   */
  public constructor(
    private readonly zone: NgZone,
    private readonly errorService: ErrorService,
    private readonly loadingService: LoadingService,
    private readonly translocoService: TranslocoService,
    private readonly chatService: ChatService,
  ) {
    skyrimtogether.on('openSettings', () => this.zone.run(() => this.openSettings.next()));
    skyrimtogether.on('init', this.onInit.bind(this));
    skyrimtogether.on('activate', this.onActivate.bind(this));
    skyrimtogether.on('deactivate', this.onDeactivate.bind(this));
    skyrimtogether.on('enterGame', this.onEnterGame.bind(this));
    skyrimtogether.on('exitGame', this.onExitGame.bind(this));
    skyrimtogether.on('openingMenu', this.onOpeningMenu.bind(this));
    skyrimtogether.on('mainMenu', state => this.zone.run(() => this.mainMenuChange.next(state)));
    skyrimtogether.on('characterCheckpoint', (required, phase, name, revision, detail, roster) => this.zone.run(() => {
      if (phase === 'choose') {
        const slots: CharacterSlot[] = [];
        for (const row of roster.split('\n')) {
          if (/^[0-9a-f]{64}/.test(row) && row.length > 64)
            slots.push({ key: row.slice(0, 64), label: row.slice(64) });
        }
        this.characterSlotsChange.next(slots);
      }
      this.checkpointChange.next({ required, phase, name, revision, detail });
      if (phase === 'request-roster') this.requestCharacterRoster();
    }));
    skyrimtogether.on('checkpointPreview', (key, encoded) => this.zone.run(() => this.onCheckpointPreview(key, encoded)));
    skyrimtogether.on('serverLobby', state => this.zone.run(() => {
      this.lobbyStateChange.next(state);
      if (state) this.isConnectionInProgressChange.next(false);
      else this.characterSlotsChange.next([]);
    }));
    skyrimtogether.on('connect', this.onConnect.bind(this));
    skyrimtogether.on('disconnect', this.onDisconnect.bind(this));
    skyrimtogether.on('setName', this.onSetName.bind(this)); //not wanted, we dont sync name changes
    skyrimtogether.on('setVersion', this.onSetVersion.bind(this));
    skyrimtogether.on('debug', this.onDebug.bind(this)); //not needed anymore
    skyrimtogether.on('debugData', this.onUpdateDebug.bind(this));
    skyrimtogether.on('playerConnected', this.onPlayerConnected.bind(this));
    skyrimtogether.on(
      'playerDisconnected',
      this.onPlayerDisconnected.bind(this),
    );
    skyrimtogether.on('setHealth', this.onSetHealth.bind(this));
    skyrimtogether.on('setLevel', this.onSetLevel.bind(this));
    skyrimtogether.on('setCell', this.onSetCell.bind(this));
    skyrimtogether.on('setPlayer3dLoaded', this.onSetPlayer3dLoaded.bind(this));
    skyrimtogether.on(
      'setPlayer3dUnloaded',
      this.onSetPlayer3dUnloaded.bind(this),
    );
    skyrimtogether.on('setLocalPlayerId', this.onSetLocalPlayerId.bind(this));
    skyrimtogether.on('protocolMismatch', this.onProtocolMismatch.bind(this));
    skyrimtogether.on('triggerError', this.onTriggerError.bind(this));
    skyrimtogether.on('dummyData', this.onDummyData.bind(this));
    skyrimtogether.on('partyInfo', this.onPartyInfo.bind(this));
    skyrimtogether.on('partyCreated', this.onPartyCreated.bind(this));
    skyrimtogether.on('partyLeft', this.onPartyLeft.bind(this));
    skyrimtogether.on(
      'partyInviteReceived',
      this.onPartyInviteReceived.bind(this),
    );
    skyrimtogether.on('travelInvitation', (id, remainingMs, count, canCancel, accepted, leader, destination) => {
      this.zone.run(() => {
        if (remainingMs > 0) {
          this.travelInvitation.next({ id, expiresAt: performance.now() + remainingMs, count, canCancel, accepted, leader, destination });
        } else if (this.travelInvitation.value?.id === id) {
          this.travelInvitation.next(undefined);
        }
      });
    });
    skyrimtogether.on('sharedDialogue', this.onSharedDialogue.bind(this));
    skyrimtogether.on('dialogueFocus', (open, actorId) => this.zone.run(() => this.dialogueFocus.next({ open, actorId })));
    skyrimtogether.on('characterPortrait', this.onCharacterPortrait.bind(this));
    skyrimtogether.on('questUpdated', this.onQuestUpdated.bind(this));
  }

  /**
   * Dispose.
   */
  public ngOnDestroy(): void {
    skyrimtogether.off('init');
    skyrimtogether.off('activate');
    skyrimtogether.off('deactivate');
    skyrimtogether.off('enterGame');
    skyrimtogether.off('exitGame');
    skyrimtogether.off('openingMenu');
    skyrimtogether.off('mainMenu');
    skyrimtogether.off('openSettings');
    skyrimtogether.off('serverLobby');
    skyrimtogether.off('characterCheckpoint');
    skyrimtogether.off('checkpointPreview');
    skyrimtogether.off('connect');
    skyrimtogether.off('disconnect');
    skyrimtogether.off('setName');
    skyrimtogether.off('setVersion');
    skyrimtogether.off('debug');
    skyrimtogether.off('debugData');
    skyrimtogether.off('playerConnected');
    skyrimtogether.off('playerDisconnected');
    skyrimtogether.off('setHealth');
    skyrimtogether.off('setLevel');
    skyrimtogether.off('setCell');
    skyrimtogether.off('setPlayer3dLoaded');
    skyrimtogether.off('setPlayer3dUnloaded');
    skyrimtogether.off('setLocalPlayerId');
    skyrimtogether.off('protocolMismatch');
    skyrimtogether.off('triggerError');
    skyrimtogether.off('dummyData');
    skyrimtogether.off('partyInfo');
    skyrimtogether.off('partyCreated');
    skyrimtogether.off('partyLeft');
    skyrimtogether.off('partyInviteReceived');
    skyrimtogether.off('sharedDialogue');
    skyrimtogether.off('travelInvitation');
    skyrimtogether.off('dialogueFocus');
    skyrimtogether.off('characterPortrait');
    skyrimtogether.off('questUpdated');
  }

  /**
   * Connect to a server at the given address and port.
   *
   * @param host IP address or hostname.
   * @param port Port.
   * @param password Password or admin password
   */
  public connect(host: string, port: number, password = ''): void {
    if (this.lobbyStateChange.getValue() || this.connectionStateChange.getValue() || this.isConnectionInProgressChange.getValue()) return;
    skyrimtogether.connect(host, port, password);
    this.isConnectionInProgressChange.next(true);
    this._host = host;
    this._port = port;
    this._password = password;
  }

  private randomKey(): string {
    return Array.from(crypto.getRandomValues(new Uint8Array(32)))
      .map(byte => byte.toString(16).padStart(2, '0')).join('');
  }

  private requestCharacterRoster(): void {
    this.characterSlotsChange.next([]);
    const endpoint = JSON.stringify([this._host?.trim().toLowerCase(), this._port]);
    let owner: string;
    try {
      const identityKey = 'st-character-owner-v1:' + endpoint;
      owner = localStorage.getItem(identityKey) ?? this.randomKey();
      if (!/^[0-9a-f]{64}$/.test(owner)) throw new Error('Invalid character identity');
      localStorage.setItem(identityKey, owner);
    } catch {
      skyrimtogether.requestCharacters('', '');
      return;
    }

    // Legacy saves have no owner metadata. Present the old private keys once the
    // server is selected. Only the server response can populate the picker.
    let claims = '';
    try {
      const old = JSON.parse(localStorage.getItem('st-character-slots-v1:' + endpoint) || '[]');
      if (Array.isArray(old)) {
        for (const slot of old.slice(0, 32)) {
          if (/^[0-9a-f]{64}$/.test(slot?.key) && typeof slot?.label === 'string' &&
              slot.label.length > 0 && slot.label.length <= 26 && !/[\x00-\x1f\x7f]/.test(slot.label))
            claims += slot.key + slot.label + '\n';
        }
      }
    } catch { /* An unreadable legacy list does not hide server-owned characters. */ }
    skyrimtogether.requestCharacters(owner, claims);
  }

  public selectCharacter(slot: CharacterSlot): void {
    if (this.lobbyStateChange.value && this.checkpointChange.value.phase === 'choose')
      skyrimtogether.selectCharacter(slot.key, slot.label, false);
  }

  public createCharacter(label: string): void {
    if (!this.lobbyStateChange.value || this.checkpointChange.value.phase !== 'choose') return;
    const name = label.trim();
    if (!name || name.length > 26 || this.characterSlotsChange.value.length >= 32) return;
    skyrimtogether.selectCharacter(this.randomKey(), name, true);
  }

  public uiReady(): void { skyrimtogether.uiReady(); }

  public enterWorld(): void {
    if (this.lobbyStateChange.getValue() && this.inGameStateChange.getValue()) skyrimtogether.enterWorld();
  }

  public disconnect(action?: 'retry' | 'cancel' | 'discard'): void {
    if (action) skyrimtogether.disconnect(action);
    else skyrimtogether.disconnect();
  }

  /**
   * Reveal players in the immediate area (lighting them up with a visual effect).
   */
  public revealPlayers(): void {
    skyrimtogether.revealPlayers();
  }

  /**
   * Launch a party.
   */
  public launchParty(): void {
    skyrimtogether.launchParty();
  }

  /**
   * Create a party invite.
   */
  public createPartyInvite(playerId: number): void {
    skyrimtogether.createPartyInvite(playerId);
  }

  /**
   * Accept a party invite.
   */
  public acceptPartyInvite(inviterId: number): void {
    skyrimtogether.acceptPartyInvite(inviterId);
  }

  /**
   * As a party leader, kick a player from the party.
   */
  public kickPartyMember(playerId: number): void {
    skyrimtogether.kickPartyMember(playerId);
  }

  /**
   * Leave a party.
   */
  public leaveParty(): void {
    skyrimtogether.leaveParty();
  }

  /**
   * As a party leader, make someone else the leader.
   */
  public changePartyLeader(playerId: number): void {
    skyrimtogether.changePartyLeader(playerId);
  }

  /**
   * Open the play guide on the wiki in the default browser.
   */
  public openPlayGuide(): void {
    skyrimtogether.openPlayGuide();
  }

  /**
   * Deactivate UI and release control.
   */
  public deactivate(): void {
    skyrimtogether.deactivate();
  }

  /**
   * Reconnect
   */
  public reconnect(): void {
    skyrimtogether.reconnect();
    this._remainingReconnectionAttempt = 0;
  }

  public respondToTravelInvitation(id: number, action: 'join' | 'decline' | 'cancel'): void {
    const invitation = this.travelInvitation.value;
    if (invitation?.id === id && invitation.expiresAt > performance.now()) {
      skyrimtogether.sendMessage(MessageTypes.GLOBAL_CHAT, `/travel ${action} ${id}`);
    }
  }

  public teleportToPlayer(playerId: number): void {
    skyrimtogether.teleportToPlayer(playerId);
  }

  /**
   * Called when the UI is first initialized.
   */
  // TODO: is this still used?
  private onInit(): void {
    this.zone.run(() => {
      this.initDone.next(undefined);
      this.initDone.complete();
    });
  }

  /**
   * Called when the UI is activated.
   */
  private onActivate(): void {
    this.zone.run(() => {
      this.activationStateChange.next(true);
    });
  }

  /**
   * Called when the UI is deactivated.
   */
  private onDeactivate(): void {
    this.zone.run(() => {
      this.activationStateChange.next(false);
    });
  }

  /**
   * Called when the player enters a game.
   */
  private onEnterGame(): void {
    this.zone.run(() => {
      this.inGameStateChange.next(true);
    });
  }

  /**
   * Called when the player exits a game.
   */
  private onExitGame(): void {
    this.zone.run(() => {
      this.sharedDialogueChange.next([]);
      this.travelInvitation.next(undefined);
      this.dialogueFocus.next({ open: false, actorId: 0 });
      this.characterPortraits.next(new Map());
      this.inGameStateChange.next(false);
    });
  }

  /**
   * Called when the player open/close a game menu
   * @param openingMenu
   */
  private onOpeningMenu(openingMenu: boolean): void {
    this.zone.run(() => {
      this.openingMenuChange.next(openingMenu);
    });
  }

  /**
   * Called when a connection is made.
   */
  private onConnect(): void {
    this.zone.run(async () => {
      this._remainingReconnectionAttempt = environment.nbReconnectionAttempts;
      this.isConnectionInProgressChange.next(false);
      this.lobbyStateChange.next(false);
      this.connectionStateChange.next(true);

      this.chatService.pushSystemMessage('SERVICE.CLIENT.CONNECTED');
    });
  }

  /**
   * Called when a connection is terminated.
   */
  private onDisconnect(isError: boolean): void {
    void this.zone.run(async () => {
      this.localPlayerId = undefined;
      this.checkpointChange.next(emptyCheckpoint);
      this.characterSlotsChange.next([]);
      this.sharedDialogueChange.next([]);
      this.travelInvitation.next(undefined);
      this.dialogueFocus.next({ open: false, actorId: 0 });
      this.characterPortraits.next(new Map());
      this.lobbyStateChange.next(false);
      this.connectionStateChange.next(false);
      this.isConnectionInProgressChange.next(false);

      if (isError && this._remainingReconnectionAttempt > 0) {
        this._remainingReconnectionAttempt--;
        this.chatService.pushSystemMessage('SERVICE.CLIENT.CONNECTION_LOST');
        this.connect(this._host, this._port, this._password);
      } else {
        this.chatService.pushSystemMessage('SERVICE.CLIENT.DISCONNECTED');
      }
    });
  }

  /**
   * Called when the player's name changes.
   *
   * @param name Player's name.
   */
  private onSetName(name: string): void {
    this.zone.run(() => {
      this.nameChange.next(name);
    });
  }

  /**
   * Called when the client's version is set.
   *
   * @param version Game's version.
   */
  private onSetVersion(version: string): void {
    version = environment.overwriteVersion || version;

    this.zone.run(() => {
      this.versionSet.next(version);
    });
  }

  getVersion(): string {
    return this.versionSet.value;
  }

  /**
   * Add listener to when the player's press the F3 key
   */
  private onDebug(isDebug: boolean): void {
    this.zone.run(() => {
      this.debugStateChange.next(isDebug);
    });
  }

  private onUpdateDebug(
    numPacketsSent: number,
    numPacketsReceived: number,
    RTT: number,
    packetLoss: number,
    sentBandwidth: number,
    receivedBandwidth: number,
  ): void {
    this.zone.run(() => {
      this.debugDataChange.next(
        new Debug(
          numPacketsSent,
          numPacketsReceived,
          RTT,
          packetLoss,
          sentBandwidth,
          receivedBandwidth,
        ),
      );
    });
  }

  private onPlayerConnected(
    playerId: number,
    username: string,
    level: number,
    cellName: string,
  ) {
    if (environment.game) {
      console.log(
        `%conPlayerConnected`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.playerConnectedChange.next(
        new Player({
          name: username,
          id: playerId,
          connected: true,
          online: true,
          isLoaded: playerId === this.localPlayerId,
          level: level,
          cellName: cellName,
        }),
      );
    });
  }

  private onPlayerDisconnected(playerId: number, username: string) {
    if (environment.game) {
      console.log(
        `%conPlayerDisconnected`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      const portraits = new Map(this.characterPortraits.getValue());
      if (portraits.delete(playerId)) this.characterPortraits.next(portraits);
      this.playerDisconnectedChange.next(
        new Player({
          name: username,
          id: playerId,
          connected: false,
        }),
      );
    });
  }

  private onSetHealth(playerId: number, health: number) {
    this.zone.run(() => {
      this.healthChange.next(new Player({ id: playerId, health: health }));
    });
  }

  private onSetLevel(playerId: number, level: number) {
    if (environment.game) {
      console.log(
        `%conSetLevel`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.levelChange.next(new Player({ id: playerId, level: level }));
    });
  }

  private onSetCell(playerId: number, cellName: string) {
    if (environment.game) {
      console.log(
        `%conSetCell`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.cellChange.next(new Player({ id: playerId, cellName: cellName }));
    });
  }

  private onSetPlayer3dLoaded(playerId: number, health: number) {
    if (environment.game) {
      console.log(
        `%conSetPlayer3dLoaded`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.isLoadedChange.next(
        new Player({ id: playerId, isLoaded: true, health: health }),
      );
    });
  }

  private onSetPlayer3dUnloaded(playerId: number) {
    if (environment.game) {
      console.log(
        `%conSetPlayer3dUnloaded`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.isLoadedChange.next(new Player({ id: playerId, isLoaded: false }));
    });
  }

  private onSetLocalPlayerId(playerId: number) {
    if (environment.game) {
      console.log(
        `%conSetLocalPlayerId`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.localPlayerId = playerId;
    });
  }

  private onProtocolMismatch() {
    this.zone.run(() => {
      this.protocolMismatchChange.next(true);
    });
  }

  private onTriggerError(rawError: string) {
    this.zone.run(() => {
      const error = JSON.parse(rawError) as ErrorEvents;
      this.triggerError.next(error);
      void this.errorService.setError(error);
    });
  }

  private onDummyData(data: Array<number>) {
    this.zone.run(() => {
      this.debugChange.next();
      /*
      for (const numb of data) {
        console.log(numb);
      }
      console.log(data);
      */
    });
  }

  public onPartyInfo(playerIds: Array<number>, leaderId: number) {
    if (environment.game) {
      console.log(
        `%conPartyInfo`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.partyInfoChange.next(
        new PartyInfo({
          playerIds: playerIds,
          leaderId: leaderId,
        }),
      );
    });
  }

  private onPartyCreated() {
    if (environment.game) {
      console.log(
        `%conPartyCreated`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.loadingService.setLoading(false);
      this.partyInfoChange.next(
        new PartyInfo({
          playerIds: [this.localPlayerId],
          leaderId: this.localPlayerId,
        }),
      );
    });
  }

  private onPartyLeft() {
    if (environment.game) {
      console.log(
        `%conPartyLeft`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.partyLeftChange.next();
    });
  }

  private onPartyInviteReceived(inviterId: number) {
    if (environment.game) {
      console.log(
        `%conPartyInviteReceived`,
        'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
        ...Array.from(arguments).map(v => JSON.stringify(v)),
      );
    }
    this.zone.run(() => {
      this.partyInviteReceivedChange.next(inviterId);
    });
  }

  public voteDialogue(actorId: number, revision: number, index: number): void {
    skyrimtogether.voteDialogue(actorId, revision, index);
  }

  public manageDialogue(panel: SharedDialoguePanel, action: number): void {
    skyrimtogether.manageDialogue(panel.actorId, panel.revision, action);
  }

  private onCheckpointPreview(key: string, encoded: string): void {
    if (this.checkpointChange.value.phase !== 'choose' || encoded.length > 55000) return;
    const slots = this.characterSlotsChange.value;
    if (!slots.some(slot => slot.key === key)) return;
    try {
      const bytes = Uint8Array.from(atob(encoded), c => c.charCodeAt(0));
      if (bytes.length < 2) return;
      const view = new DataView(bytes.buffer);
      const headerLength = view.getUint16(0, true);
      const end = 2 + headerLength;
      if (headerLength > 4096 || end > bytes.length || (bytes.length !== end && bytes.length !== end + 96 * 96 * 4)) return;
      const preview: Partial<CharacterSlot> = {};
      if (headerLength >= 25) {
        try {
          const decoder = new TextDecoder();
          if (decoder.decode(bytes.subarray(2, 15)) !== 'TESV_SAVEGAME') throw new Error('Invalid save header');
          let pos = 27;
          const readString = () => {
            if (pos + 2 > end) throw new Error('Invalid save header');
            const length = view.getUint16(pos, true); pos += 2;
            if (pos + length > end) throw new Error('Invalid save header');
            const value = decoder.decode(bytes.subarray(pos, pos + length)); pos += length;
            return value;
          };
          const name = readString();
          if (pos + 4 > end) throw new Error('Invalid save header');
          const level = view.getUint32(pos, true); pos += 4;
          const location = readString();
          const playtime = readString();
          const raceId = readString().replace(/Race$/i, '');
          const races: Record<string, string> = { HighElf: 'High Elf', DarkElf: 'Dark Elf', WoodElf: 'Wood Elf', Orc: 'Orc', Nord: 'Nord', Breton: 'Breton', Imperial: 'Imperial', Redguard: 'Redguard', Argonian: 'Argonian', Khajiit: 'Khajiit' };
          Object.assign(preview, { name, level, location, playtime, race: races[raceId] || raceId });
        } catch { /* unsupported ESS header: retain the slot label */ }
      }
      if (bytes.length === end + 96 * 96 * 4) {
        const canvas = document.createElement('canvas');
        canvas.width = canvas.height = 96;
        const context = canvas.getContext('2d');
        if (context) {
          const image = context.createImageData(96, 96);
          const linear = (c: number) => c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
          const srgb = (c: number) => c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055;
          for (let i = 0; i < image.data.length; i += 4) {
            const alpha = bytes[end + i + 3] / 255;
            for (let c = 0; c < 3; ++c)
              image.data[i + c] = alpha > 0 ? Math.round(srgb(Math.min(1, linear(bytes[end + i + c] / 255) / alpha)) * 255) : 0;
            image.data[i + 3] = bytes[end + i + 3];
          }
          context.putImageData(image, 0, 0);
          preview.portrait = canvas.toDataURL('image/png');
        }
      }
      this.characterSlotsChange.next(this.characterSlotsChange.value.map(slot => slot.key === key ? { ...slot, ...preview } : slot));
    } catch { /* previews must never block character selection */ }
  }

  private onCharacterPortrait(playerId: number, width: number, height: number, rgba: string): void {
    if (!Number.isInteger(playerId) || playerId < 0 || !this.connectionStateChange.getValue()) return;
    this.zone.run(() => {
      const portraits = new Map(this.characterPortraits.getValue());
      if (!rgba) {
        portraits.delete(playerId);
        this.characterPortraits.next(portraits);
        return;
      }
      if (width !== 96 || height !== 96 || rgba.length !== Math.ceil(width * height * 4 / 3) * 4) return;
      try {
        const pixels = atob(rgba);
        if (pixels.length !== width * height * 4) return;
        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        const context = canvas.getContext('2d');
        if (!context) return;
        const image = context.createImageData(width, height);
        // The renderer supplies sRGB pixels premultiplied in linear light.
        const linear = (c: number) => c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
        const srgb = (c: number) => c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055;
        for (let i = 0; i < pixels.length; i += 4) {
          const alpha = pixels.charCodeAt(i + 3) / 255;
          for (let channel = 0; channel < 3; ++channel) {
            image.data[i + channel] = alpha > 0
              ? Math.round(srgb(Math.min(1, linear(pixels.charCodeAt(i + channel) / 255) / alpha)) * 255) : 0;
          }
          image.data[i + 3] = pixels.charCodeAt(i + 3);
        }
        context.putImageData(image, 0, 0);
        if (!portraits.has(playerId) && portraits.size >= 32) {
          const oldest = portraits.keys().next();
          if (!oldest.done) portraits.delete(oldest.value);
        }
        portraits.set(playerId, canvas.toDataURL('image/png'));
        this.characterPortraits.next(portraits);
      } catch {
        // A failed snapshot leaves the count and native choice available.
      }
    });
  }

  private onSharedDialogue(actorId: number, visible: boolean, owner: string,
    canChoose: boolean, revision: number, npc: string, options: string[], votes: number[], selected: number,
    canVote: boolean, canPass: boolean, isPrivate: boolean, voterIds: number[] = [], voterOptions: number[] = [], remote = false, listening = false, subtitle = ''): void {
    this.zone.run(() => {
      const panels = this.sharedDialogueChange.getValue().filter(panel => panel.actorId !== actorId);
      if (visible) {
        panels.push({ actorId, owner, canChoose, canVote, canPass, isPrivate, revision, npc, options, votes, voterIds, voterOptions, selected, remote, listening, subtitle });
      }
      panels.sort((a, b) => Number(b.canChoose) - Number(a.canChoose) || a.actorId - b.actorId);
      this.sharedDialogueChange.next(panels);
    });
  }

  private onQuestUpdated() {
    this.zone.run(() => {
      this.questUpdatedChange.next();
    });
  }
}
