import {
  AfterViewInit,
  Component,
  ElementRef,
  EventEmitter,
  HostListener,
  OnDestroy,
  Output,
  ViewChild,
} from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { firstValueFrom, Subscription } from 'rxjs';
import { View } from '../../models/view.enum';
import { ClientService } from '../../services/client.service';
import { ErrorService } from '../../services/error.service';
import { Sound, SoundService } from '../../services/sound.service';
import { StoreService } from '../../services/store.service';
import { UiRepository } from '../../store/ui.repository';

@Component({
  selector: 'app-connect',
  templateUrl: './connect.component.html',
  styleUrls: ['./connect.component.scss'],
})
export class ConnectComponent implements OnDestroy, AfterViewInit {
  public address = '';
  public password = '';
  public savePassword = false;
  public hidePassword = true;

  public connecting = false;

  @Output() public done = new EventEmitter<void>();
  @Output() public cancelled = new EventEmitter<void>();

  private focusTimer?: ReturnType<typeof setTimeout>;

  public constructor(
    private readonly client: ClientService,
    private readonly sound: SoundService,
    private readonly errorService: ErrorService,
    private readonly storeService: StoreService,
    private readonly translocoService: TranslocoService,
    private readonly uiRepository: UiRepository,
  ) {
    this.connectionSubscription = this.client.connectionStateChange.subscribe(
      state => {
        if (this.connecting) {
          this.connecting = false;

          if (state) {
            this.sound.play(Sound.Success);
            this.done.next();
          }
        }
      },
    );

    this.lobbySubscription = this.client.lobbyStateChange.subscribe(state => {
      if (state && this.connecting) {
        this.connecting = false;
        this.sound.play(Sound.Success);
        this.done.next();
      }
    });

    this.protocolMismatchSubscription =
      this.client.protocolMismatchChange.subscribe(async state => {
        if (state) {
          this.connecting = false;
          const message = await firstValueFrom(
            this.translocoService.selectTranslate<string>(
              'COMPONENT.CONNECT.ERROR.VERSION_MISMATCH',
            ),
          );
          await this.errorService.setError(message);
        }
      });

    this.address = this.storeService.get('last_connected_address', '');
    this.password = this.storeService.get('last_connected_password', '');
    this.savePassword = this.password !== '';
  }

  public ngAfterViewInit(): void {
    this.focusTimer = setTimeout(() => {
      this.inputRef.nativeElement.focus();
    }, 100);
  }

  public ngOnDestroy(): void {
    clearTimeout(this.focusTimer);
    this.connectionSubscription.unsubscribe();
    this.lobbySubscription.unsubscribe();
    this.protocolMismatchSubscription.unsubscribe();
  }

  async connect(): Promise<void> {
    const address = this.address.trim().match(/^(.+?)(?::([0-9]+))?$/);

    if (!address) {
      this.sound.play(Sound.Fail);
      const message = await firstValueFrom(
        this.translocoService.selectTranslate(
          'COMPONENT.CONNECT.ERROR.INVALID_ADDRESS',
        ),
      );
      await this.errorService.setError(message);
      return;
    }

    this.connecting = true;

    this.storeService.set('last_connected_address', this.address);
    if (this.savePassword) {
      this.storeService.set('last_connected_password', this.password);
    } else {
      this.storeService.remove('last_connected_password');
    }

    this.sound.play(Sound.Ok);
    this.client.connect(
      address[1],
      address[2] ? Number.parseInt(address[2]) : 10578,
      this.password,
    );
  }

  public cancel(): void {
    if (this.connecting) return;
    this.sound.play(Sound.Cancel);
    this.cancelled.emit();
  }

  public openServerList(): void {
    this.uiRepository.openView(View.SERVER_LIST);
  }

  @ViewChild('input')
  private inputRef!: ElementRef;

  private connectionSubscription: Subscription;
  private lobbySubscription: Subscription;

  private protocolMismatchSubscription: Subscription;

  @HostListener('window:keydown.escape', ['$event'])
  // @ts-ignore
  private activate(event: KeyboardEvent): void {
    if (this.errorService.getError()) {
      this.errorService.removeError();
    } else {
      this.cancel();
    }

    event.stopPropagation();
    event.preventDefault();
  }
}
