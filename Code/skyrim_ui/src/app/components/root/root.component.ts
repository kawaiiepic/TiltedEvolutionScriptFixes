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
  // Temporary visual test, local only. Remove or disable after checking the layout.
  private readonly fakeDialogueSuggestions = true;
  private fakeSuggestionActor = 0;
  private fakeSuggestionKey = '';
  private fakeSuggestionIndex = -1;
  focusedDialogue$ = combineLatest([this.sharedDialogues$, this.dialogueFocus$]).pipe(
    map(([panels, focus]) => {
      const panel = focus.open ? panels.find(candidate => candidate.actorId === focus.actorId) : undefined;
      if (!focus.open || this.fakeSuggestionActor !== focus.actorId) {
        this.fakeSuggestionActor = focus.open ? focus.actorId : 0;
        this.fakeSuggestionKey = '';
        this.fakeSuggestionIndex = -1;
      }
      if (!panel || !this.fakeDialogueSuggestions || !panel.canChoose || panel.isPrivate) return panel;
      const eligible = panel.options.map((option, index) => option.trim() ? index : -1).filter(index => index >= 0);
      if (!eligible.length) {
        this.fakeSuggestionKey = '';
        this.fakeSuggestionIndex = -1;
        return panel;
      }
      const key = JSON.stringify(panel.options);
      if (key !== this.fakeSuggestionKey || this.fakeSuggestionIndex < 0) {
        this.fakeSuggestionKey = key;
        this.fakeSuggestionIndex = eligible[Math.floor(Math.random() * eligible.length)];
      }
      const votes = panel.options.map((_, index) => panel.votes[index] || 0);
      votes[this.fakeSuggestionIndex]++;
      // Reuse the owner's cached portrait if available, otherwise use a labeled initial.
      // This is a display-only clone: never submit a fake vote to the server.
      return { ...panel, votes, voterIds: [...panel.voterIds, -1], voterOptions: [...panel.voterOptions, this.fakeSuggestionIndex] };
    }),
  );
  portraits$ = this.client.characterPortraits.asObservable().pipe(map(portraits => {
    const local = this.client.localPlayerId;
    const image = local === undefined ? undefined : portraits.get(local);
    if (!image) return portraits;
    const display = new Map(portraits);
    display.set(-1, image);
    return display;
  }));

  trackDialogue(_index: number, panel: SharedDialoguePanel): number { return panel.actorId; }

  dialogueVoterIds(panel: SharedDialoguePanel, index: number): number[] {
    return panel.voterIds.filter((_, i) => panel.voterOptions[i] === index).slice(0, 8);
  }

  dialogueVoterName(id: number): string {
    if (id === -1) return 'Test player';
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
