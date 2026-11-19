import { Overlay } from '@angular/cdk/overlay';
import { Component, OnInit, ViewChild } from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { combineLatest, takeUntil } from 'rxjs';
import { environment } from '../../../environments/environment';
import { fadeInOutActiveAnimation } from '../../animations/fade-in-out-active.animation';
import { View } from '../../models/view.enum';
import { ClientService, SharedDialoguePanel } from '../../services/client.service';
import { DestroyService } from '../../services/destroy.service';
import { PlayerListService } from '../../services/player-list.service';
import {
  SettingService,
  fontSizeToPixels,
} from '../../services/setting.service';
import { Sound, SoundService } from '../../services/sound.service';
import { UiRepository } from '../../store/ui.repository';
import { ChatComponent } from '../chat/chat.component';
import { GroupComponent } from '../group/group.component';
import { controlsAnimation } from './controls.animation';
import { notificationsAnimation } from './notifications.animation';
import { map } from 'rxjs/operators';

const REVEAL_EFFECT_DURATION_MS = 10000 // todo: pass value from C++?

@Component({
  selector: 'app-root',
  templateUrl: './root.component.html',
  styleUrls: ['./root.component.scss'],
  animations: [
    controlsAnimation,
    fadeInOutActiveAnimation,
    notificationsAnimation,
  ],
  host: { 'data-app-root-game': environment.game.toString() },
  providers: [DestroyService],
})
export class RootComponent implements OnInit {
  /* ### ENUMS ### */
  readonly RootView = View;

  view$ = this.uiRepository.view$;

  dialogueFocus$ = this.client.dialogueFocus.asObservable();
  sharedDialogues$ = this.client.sharedDialogueChange.asObservable();
  focusedDialogue$ = combineLatest([this.sharedDialogues$, this.dialogueFocus$]).pipe(
    map(([panels, focus]) => focus.open ? panels.find(panel => panel.actorId === focus.actorId) : undefined),
  );
  portraits$ = this.client.characterPortraits.asObservable();
  dialoguePortraits$ = this.settingService.settings.dialoguePortraits.asObservable();
  remoteDialogueNotice$ = combineLatest([this.sharedDialogues$, this.settingService.settings.remoteDialogueNotices]).pipe(
    map(([panels, enabled]) => enabled ? panels.find(panel => panel.remote && !panel.listening) : undefined),
  );
  private readonly autoListenAttempts = new Map<number, number>();
  private readonly stoppedListening = new Set<number>();

  trackDialogue(_index: number, panel: SharedDialoguePanel): number { return panel.actorId; }

  dialogueVoterIds(panel: SharedDialoguePanel, index: number): number[] {
    return panel.voterIds.filter((_, i) => panel.voterOptions[i] === index).slice(0, 8);
  }

  dialogueVoterName(id: number): string {
    if (id === this.client.localPlayerId) return 'You';
    return this.playerList.playerList.getValue()?.players.find(player => player.id === id)?.name || `Player ${id}`;
  }

  dialogueVoters(panel: SharedDialoguePanel, index: number): string {
    const players = this.playerList.playerList.getValue()?.players || [];
    return panel.voterIds
      .filter((_, i) => panel.voterOptions[i] === index)
      .map(id => id === this.client.localPlayerId ? 'You' : players.find(player => player.id === id)?.name || `Player ${id}`)
      .join(', ');
  }

  voteDialogue(panel: SharedDialoguePanel, index: number): void {
    if (panel.canVote) this.client.voteDialogue(panel.actorId, panel.revision, index);
  }

  setDialogueListening(panel: SharedDialoguePanel, listening: boolean): void {
    if (listening) this.stoppedListening.delete(panel.actorId);
    else this.stoppedListening.add(panel.actorId);
    this.client.manageDialogue(panel, listening ? 3 : 4);
  }

  passDialogue(panel: SharedDialoguePanel): void {
    if (panel.canPass) this.client.manageDialogue(panel, 0);
  }

  toggleDialoguePrivacy(panel: SharedDialoguePanel): void {
    if (panel.canChoose) this.client.manageDialogue(panel, panel.isPrivate ? 2 : 1);
  }

  connected$ = this.client.connectionStateChange.asObservable();
  mainMenu$ = this.client.mainMenuChange.asObservable();
  lobby$ = this.client.lobbyStateChange.asObservable();
  checkpoint$ = this.client.checkpointChange.asObservable();
  characterSlots$ = this.client.characterSlotsChange.asObservable();
  characterLabel = '';
  selectCharacter(slot: import('../../services/client.service').CharacterSlot): void { this.client.selectCharacter(slot); }
  createCharacter(): void { this.client.createCharacter(this.characterLabel); }

  enterWorld(): void { this.client.enterWorld(); }
  returnToNativeMenu(): void { this.client.deactivate(); }
  menuOpen$ = this.client.openingMenuChange.asObservable();
  inGame$ = this.client.inGameStateChange.asObservable();
  active$ = this.client.activationStateChange.asObservable();
  connectionInProgress$ = this.client.isConnectionInProgressChange.asObservable();
  revealingInProgress$ = false;

  @ViewChild('chat') private chatComp!: ChatComponent;
  @ViewChild(GroupComponent) private groupComponent: GroupComponent;

  public constructor(
    private readonly destroy$: DestroyService,
    private readonly client: ClientService,
    private readonly playerList: PlayerListService,
    private readonly sound: SoundService,
    private readonly uiRepository: UiRepository,
    private readonly translocoService: TranslocoService,
    private readonly settingService: SettingService,
    public readonly overlay: Overlay, // used for mockup
  ) {
    this.translocoService.setActiveLang(
      this.settingService.settings.language.getValue(),
    );
  }

  public ngOnInit(): void {
    this.onInGameStateSubscription();
    this.onActivationStateSubscription();
    this.onFontSizeSubscription();
    this.client.openSettings.pipe(takeUntil(this.destroy$)).subscribe(() => this.setView(View.SETTINGS));
    combineLatest([this.sharedDialogues$, this.active$, this.menuOpen$, this.view$, this.settingService.settings.autoListenDialogue])
      .pipe(takeUntil(this.destroy$))
      .subscribe(([panels, active, menuOpen, view, enabled]) => {
        const actorIds = new Set(panels.map(panel => panel.actorId));
        for (const id of this.autoListenAttempts.keys()) {
          if (!actorIds.has(id)) this.autoListenAttempts.delete(id);
        }
        for (const id of this.stoppedListening) {
          if (!actorIds.has(id)) this.stoppedListening.delete(id);
        }
        if (!enabled || !active || menuOpen || view || panels.some(panel => panel.listening)) return;
        const offer = panels.find(panel => panel.remote && !panel.listening &&
          !this.stoppedListening.has(panel.actorId));
        if (!offer || this.autoListenAttempts.get(offer.actorId) === offer.revision) return;
        this.autoListenAttempts.set(offer.actorId, offer.revision);
        this.setDialogueListening(offer, true);
      });
    this.client.uiReady();
  }

  public onInGameStateSubscription() {
    this.client.inGameStateChange
      .pipe(takeUntil(this.destroy$))
      .subscribe(state => {
        if (!state) {
          this.closeView();
        }
      });
  }

  public onActivationStateSubscription() {
    this.client.activationStateChange
      .pipe(takeUntil(this.destroy$))
      .subscribe(state => {
        if (
          this.client.inGameStateChange.getValue() &&
          state &&
          !this.uiRepository.isViewOpen()
        ) {
          setTimeout(() => this.chatComp?.focus(), 100);
        }
        if (state && this.client.mainMenuChange.getValue() &&
            !this.client.lobbyStateChange.getValue() && !this.client.connectionStateChange.getValue() &&
            !this.client.isConnectionInProgressChange.getValue()) {
          this.setView(View.CONNECT);
        }
        if (!state) {
          this.closeView();
        }
      });
  }

  public onFontSizeSubscription() {
    this.settingService.settings.fontSize
      .pipe(
        takeUntil(this.destroy$),
        map(size => fontSizeToPixels[size]),
      )
      .subscribe(size => {
        document.documentElement.setAttribute('style', `font-size: ${size}px;`);
      });
  }

  public setView(view: View | null) {
    this.uiRepository.openView(view);

    if (view) {
      this.sound.play(Sound.Focus);
    } else if (this.chatComp) {
      this.chatComp.focus();
    }
  }

  public resolveDisconnect(action: 'retry' | 'cancel' | 'discard'): void {
    this.client.disconnect(action);
  }

  public cancelConnection(): void {
    this.client.disconnect();
  }

  public closeTitleView(): void {
    if (this.client.mainMenuChange.getValue()) {
      this.returnToNativeMenu();
    } else {
      this.closeView();
    }
  }

  public closeView() {
    this.uiRepository.openView(null);
  }

  public reconnect(): void {
    this.client.reconnect();
  }

  public revealPlayers(): void {
    if (this.revealingInProgress$)
      return;

    this.revealingInProgress$ = true;
    setTimeout(() => { this.revealingInProgress$ = false }, REVEAL_EFFECT_DURATION_MS);

    this.sound.play(Sound.Focus);
    this.client.revealPlayers();
  }
}
